// Copyright 2026 Black-Seraph
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <windows.h>
#include <wdf.h>
// NOTE: deliberately no <initguid.h> here — Device.c emits the GUID/DEVPKEY data
// under INITGUID; this TU only needs the extern declarations to avoid LNK2005.
#include <devpkey.h>
#include <hidport.h>     // HID_DESCRIPTOR / HID_DEVICE_ATTRIBUTES / HID_XFER_PACKET used by Device.h
#include "Device.h"      // brings in Public.h (which has no include guard — do not include it directly)
#include "Queue.h"
#include "SharedMemoryServer.h"
#include "Xusb.h"

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL XusbEvtIoDeviceControl;

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

BOOLEAN XusbIsXboxDevice(_In_ PWDFDEVICE_INIT DeviceInit)
{
	WCHAR ids[512];
	ULONG resultLength = 0;

	RtlZeroMemory(ids, sizeof(ids));

	NTSTATUS status = WdfFdoInitQueryProperty(
		DeviceInit,
		DevicePropertyHardwareID,
		sizeof(ids) - sizeof(WCHAR),
		(PVOID)ids,
		&resultLength);

	if (!NT_SUCCESS(status))
	{
		return FALSE;
	}

	// The Xbox controller is installed with hardware id ROOT\VID_045E&PID_02FF&IG_00.
	return (wcsstr(ids, L"PID_02FF") != NULL);
}

// ---------------------------------------------------------------------------
// HID report -> XUSB gamepad state translation
//
// The client library keeps sending the same 64-byte Xbox HID report (see
// XboxSendRawInput in DuoController.c); we translate it here. Byte layout:
//   [0]      report id
//   [1..2]   LX  u16  (0=left  .. 65535=right)
//   [3..4]   LY  u16  (0=top   .. 65535=bottom)
//   [5..6]   RX  u16
//   [7..8]   RY  u16
//   [9..10]  LT  10-bit (LeftTrigger << 2)
//   [11..12] RT  10-bit (RightTrigger << 2)
//   [13]     DPad 0=none,1=N,2=NE,3=E,4=SE,5=S,6=SW,7=W,8=NW
//   [14]     bit0 A,1 B,2 X,3 Y,4 LB,5 RB,6 Back,7 Start
//   [15]     bit0 LSB,1 RSB,2 Guide,3 P1,4 P2,5 P3,6 P4
// ---------------------------------------------------------------------------

static VOID XusbTranslateReport(_In_reads_bytes_(XB1_REPORT_SIZE) const UCHAR* r, _Out_ XUSB_OUT_STATE_0101* s)
{
	USHORT btn = 0;

	if (r[14] & (1u << 0)) btn |= XI_A;
	if (r[14] & (1u << 1)) btn |= XI_B;
	if (r[14] & (1u << 2)) btn |= XI_X;
	if (r[14] & (1u << 3)) btn |= XI_Y;
	if (r[14] & (1u << 4)) btn |= XI_LEFT_SHOULDER;
	if (r[14] & (1u << 5)) btn |= XI_RIGHT_SHOULDER;
	if (r[14] & (1u << 6)) btn |= XI_BACK;
	if (r[14] & (1u << 7)) btn |= XI_START;

	if (r[15] & (1u << 0)) btn |= XI_LEFT_THUMB;
	if (r[15] & (1u << 1)) btn |= XI_RIGHT_THUMB;
	if (r[15] & (1u << 2)) btn |= XI_GUIDE;
	// Paddles (r[15] bits 3..6) have no XInput representation and are dropped,
	// matching real Elite behaviour through the XInput API.

	switch (r[13])
	{
	case 1: btn |= XI_DPAD_UP; break;
	case 2: btn |= XI_DPAD_UP | XI_DPAD_RIGHT; break;
	case 3: btn |= XI_DPAD_RIGHT; break;
	case 4: btn |= XI_DPAD_DOWN | XI_DPAD_RIGHT; break;
	case 5: btn |= XI_DPAD_DOWN; break;
	case 6: btn |= XI_DPAD_DOWN | XI_DPAD_LEFT; break;
	case 7: btn |= XI_DPAD_LEFT; break;
	case 8: btn |= XI_DPAD_UP | XI_DPAD_LEFT; break;
	default: break;
	}

	s->wButtons = btn;

	USHORT ltRaw = (USHORT)(r[9] | (r[10] << 8));   // 0..1023
	USHORT rtRaw = (USHORT)(r[11] | (r[12] << 8));
	s->bLeftTrigger = (UCHAR)(ltRaw >> 2);          // -> 0..255
	s->bRightTrigger = (UCHAR)(rtRaw >> 2);

	int lx = (int)(USHORT)(r[1] | (r[2] << 8));
	int ly = (int)(USHORT)(r[3] | (r[4] << 8));
	int rx = (int)(USHORT)(r[5] | (r[6] << 8));
	int ry = (int)(USHORT)(r[7] | (r[8] << 8));

	// HID 0..65535 (Y axis points down) -> XInput signed -32768..32767 (Y points up)
	s->sThumbLX = (SHORT)(lx - 32768);
	s->sThumbLY = (SHORT)(32767 - ly);
	s->sThumbRX = (SHORT)(rx - 32768);
	s->sThumbRY = (SHORT)(32767 - ry);
}

// ---------------------------------------------------------------------------
// Device creation
// ---------------------------------------------------------------------------

NTSTATUS XusbCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
	WDF_OBJECT_ATTRIBUTES attributes;
	PDEVICE_CONTEXT ctx;
	WDFDEVICE device;
	WDF_IO_QUEUE_CONFIG queueConfig;
	NTSTATUS status;

	// A plain function driver (NOT a filter): this device owns its devnode and is the
	// target of the XUSB IOCTLs. Reuses DEVICE_CONTEXT so the shared-memory input
	// server (and its cleanup) work unchanged.
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
	attributes.EvtCleanupCallback = DuoControllerEvtDeviceCleanupCallback;

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	ctx = DeviceGetContext(device);
	ctx->Device = device;
	ctx->ControllerSubType = DuoControllerSubTypeXbox;
	ctx->XusbPacketNumber = 0;
	RtlZeroMemory(ctx->XusbLastReport, sizeof(ctx->XusbLastReport));

	// NOTE: unlike the HID path, the XUSB device is deliberately NOT session-isolated.
	// XInput is a global API — XInputGetState exposes the same 4 slots to every session,
	// so a session-isolated XUSB device is a contradiction. ViGEmBus, the reference
	// XUSB provider, likewise does not isolate; its pads still reach every session because
	// device interfaces are globally visible. So we skip the DEVPKEY_Device_SessionId
	// assignment entirely here.
	// (The 0xc0000182 start failures once blamed on isolation were actually the INF
	// putting WUDFRd in LowerFilters on a devnode WUDFRd already services — see the
	// [DuoController_Install_Xbox.NT.hw] comment in DuoController.inf.)

	// Capture the device instance id — the shared-memory server derives its object
	// names from it, and the client library uses the same id to find them.
	{
		WDF_DEVICE_PROPERTY_DATA propertyData = { 0 };
		propertyData.Size = sizeof(propertyData);
		propertyData.PropertyKey = &DEVPKEY_Device_InstanceId;
		WDFMEMORY idMemory;
		DEVPROPTYPE propertyType;
		status = WdfDeviceAllocAndQueryPropertyEx(device, &propertyData, NonPagedPool,
			WDF_NO_OBJECT_ATTRIBUTES, (WDFMEMORY*)&idMemory, &propertyType);
		if (!NT_SUCCESS(status))
		{
			return status;
		}

		WCHAR* instanceId = (WCHAR*)WdfMemoryGetBuffer(idMemory, NULL);
		if (instanceId == NULL)
		{
			WdfObjectDelete(idMemory);
			return STATUS_UNSUCCESSFUL;
		}

		wcscpy_s(ctx->DeviceInstanceId, MAX_DEVICE_INSTANCE_ID_LEN, instanceId);

		WCHAR* serialStart = wcsrchr(instanceId, L'\\');
		if (serialStart != NULL)
		{
			serialStart++;
			wcscpy_s(ctx->SerialNumber, HID_DEVICE_SERIAL_NUMBER_MAX_LEN, serialStart);
		}
		else
		{
			wcscpy_s(ctx->SerialNumber, HID_DEVICE_SERIAL_NUMBER_MAX_LEN, instanceId);
		}

		WdfObjectDelete(idMemory);
	}

	// Advertise the XUSB device interface so XInput/GameInput enumerate us.
	status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_XUSB, NULL);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	// Default parallel queue receives the XUSB IOCTL traffic.
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
	queueConfig.EvtIoDeviceControl = XusbEvtIoDeviceControl;
	status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->DefaultQueue);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	// The shared-memory input worker resolves the device context via a manual queue,
	// so create one for it (no reads pend here; it is used only for that lookup).
	status = DuoControllerManualQueueInitialize(device, &ctx->ManualQueue);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	// Stand up the shared-memory server that receives input reports from the client.
	// NOTE: CreateSharedMemoryServer returns 0 on SUCCESS, >0 on failure (same
	// convention as the HID path in DuoControllerCreateDevice).
	if (CreateSharedMemoryServer(device) > 0)
	{
		return STATUS_UNSUCCESSFUL;
	}

	return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// XUSB IOCTL handler
// ---------------------------------------------------------------------------

VOID XusbEvtIoDeviceControl(
	_In_ WDFQUEUE Queue,
	_In_ WDFREQUEST Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode)
{
	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
	size_t bytesReturned = 0;
	PVOID outBuf = NULL;
	size_t outLen = 0;

	WDFDEVICE device = WdfIoQueueGetDevice(Queue);
	PDEVICE_CONTEXT ctx = DeviceGetContext(device);

	switch (IoControlCode)
	{
	case IOCTL_XINPUT_GET_INFORMATION:
	{
		status = WdfRequestRetrieveOutputBuffer(Request, sizeof(XUSB_OUT_DEVICE_INFO), &outBuf, &outLen);
		if (NT_SUCCESS(status))
		{
			XUSB_OUT_DEVICE_INFO info;
			RtlZeroMemory(&info, sizeof(info));
			info.XUSBVersion = XUSB_VERSION_1_1;
			info.DeviceCount = 1;
			info.Flags = 0;
			info.VendorId = XUSB_XBOX_VID;
			info.ProductId = XUSB_XBOX_PID;
			RtlCopyMemory(outBuf, &info, sizeof(info));
			bytesReturned = sizeof(info);
		}
		break;
	}

	case IOCTL_XINPUT_GET_LED_STATE:
	{
		status = WdfRequestRetrieveOutputBuffer(Request, sizeof(XUSB_OUT_LED), &outBuf, &outLen);
		if (NT_SUCCESS(status))
		{
			XUSB_OUT_LED led;
			RtlZeroMemory(&led, sizeof(led));
			led.XUSBVersion = XUSB_VERSION_1_1;
			led.LEDState = XINPUT_LED_1;
			RtlCopyMemory(outBuf, &led, sizeof(led));
			bytesReturned = sizeof(led);
		}
		break;
	}

	case IOCTL_XINPUT_GET_CAPABILITIES:
	{
		status = WdfRequestRetrieveOutputBuffer(Request, sizeof(XUSB_OUT_CAPS_0101), &outBuf, &outLen);
		if (NT_SUCCESS(status))
		{
			XUSB_OUT_CAPS_0101 caps;
			RtlZeroMemory(&caps, sizeof(caps));
			caps.XUSBVersion = XUSB_VERSION_1_1;
			caps.Type = XINPUT_DEVTYPE_GAMEPAD;
			caps.SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
			caps.wButtons = XINPUT_BUTTON_MASK_WITHOUT_GUIDE;
			caps.bLeftTrigger = 0xFF;
			caps.bRightTrigger = 0xFF;
			caps.sThumbLX = (SHORT)-64;
			caps.sThumbLY = (SHORT)-64;
			caps.sThumbRX = (SHORT)-64;
			caps.sThumbRY = (SHORT)-64;
			caps.bLeftMotorSpeed = 0xFF;
			caps.bRightMotorSpeed = 0xFF;
			RtlCopyMemory(outBuf, &caps, sizeof(caps));
			bytesReturned = sizeof(caps);
		}
		break;
	}

	case IOCTL_XINPUT_GET_GAMEPAD_STATE:
	{
		status = WdfRequestRetrieveOutputBuffer(Request, sizeof(XUSB_OUT_STATE_0101), &outBuf, &outLen);
		if (NT_SUCCESS(status))
		{
			XUSB_OUT_STATE_0101 state;
			RtlZeroMemory(&state, sizeof(state));
			state.XUSBVersion = XUSB_VERSION_1_1;
			state.Status = 1;   // connected

			// Bump the packet number only when the input actually changed, matching
			// XInput's dwPacketNumber contract.
			if (RtlCompareMemory(ctx->XusbLastReport, ctx->InputReport, XB1_REPORT_SIZE) != XB1_REPORT_SIZE)
			{
				ctx->XusbPacketNumber++;
				RtlCopyMemory(ctx->XusbLastReport, ctx->InputReport, XB1_REPORT_SIZE);
			}
			state.dwPacketNumber = ctx->XusbPacketNumber;

			XusbTranslateReport(ctx->InputReport, &state);

			RtlCopyMemory(outBuf, &state, sizeof(state));
			bytesReturned = sizeof(state);
		}
		break;
	}

	case IOCTL_XINPUT_SET_GAMEPAD_STATE:
	{
		// LED assignment / rumble. Vibration is forwarded to the client through the
		// shared-memory output channel as the same PID-shaped report the HID path's
		// SetOutputReport emits ([0]=report id, [4]/[5]=left/right magnitude); the
		// client's XboxFfbThreadProc translates it into the vibration callback.
		UNREFERENCED_PARAMETER(InputBufferLength);
		PVOID inBuf = NULL;
		size_t inLen = 0;
		if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(XUSB_IN_SET_STATE), &inBuf, &inLen)))
		{
			XUSB_IN_SET_STATE* set = (XUSB_IN_SET_STATE*)inBuf;
			if (set->Flags & XUSB_SET_STATE_FLAG_VIBRATION)
			{
				PSHARED_MEMORY_SERVER_ATTRIBUTES attr = &ctx->SharedMemServerAttributes;
				if (attr->OutputView != NULL &&
					WaitForSingleObject(attr->StopEvent, 0) != WAIT_OBJECT_0)
				{
					PBYTE outputView = (PBYTE)attr->OutputView;
					outputView[0] = XB1_OUTPUT_REPORT_ID;
					outputView[1] = 0x0C;                  // PID enable nibble: left+right motors
					outputView[2] = 0;                     // trigger haptics: none in XInput
					outputView[3] = 0;
					outputView[4] = set->LeftMotorSpeed;
					outputView[5] = set->RightMotorSpeed;
					outputView[6] = 0;                     // duration/delay/loop zeroed, like the DS path
					outputView[7] = 0;
					outputView[8] = 0;
					SetEvent(attr->OutputEvent);
				}
			}
		}
		// Always succeed: XInput's slot assignment depends on SetState succeeding.
		status = STATUS_SUCCESS;
		bytesReturned = 0;
		break;
	}

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		bytesReturned = 0;
		break;
	}

	if (bytesReturned > OutputBufferLength)
	{
		bytesReturned = OutputBufferLength;
	}

	WdfRequestCompleteWithInformation(Request, status, (ULONG_PTR)bytesReturned);
}
