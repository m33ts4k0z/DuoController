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
#include "Public.h"      // extern decl of GUID_DEVINTERFACE_XUSB (data emitted by Device.c via initguid)
#include "XusbSpike.h"

// ---------------------------------------------------------------------------
// XUSB IOCTL protocol
//
// Control codes and buffer layouts derived from the XInput client that talks to
// them (Nemirtingas/OpenXinput, a faithful reimplementation of Microsoft's
// xinput1_x.dll). This driver is the *responder* side, which is otherwise
// unpublished: ViGEmBus emulates at the USB layer and lets Microsoft's xusb22.sys
// answer these — here we answer them directly from user mode.
// ---------------------------------------------------------------------------

#define IOCTL_XINPUT_GET_INFORMATION          0x80006000  // CTL_CODE(0x8000,0x800,BUFFERED,READ)
#define IOCTL_XINPUT_GET_CAPABILITIES         0x8000E004  // CTL_CODE(0x8000,0x801,BUFFERED,READ|WRITE)
#define IOCTL_XINPUT_GET_LED_STATE            0x8000E008  // CTL_CODE(0x8000,0x802,BUFFERED,READ|WRITE)
#define IOCTL_XINPUT_GET_GAMEPAD_STATE        0x8000E00C  // CTL_CODE(0x8000,0x803,BUFFERED,READ|WRITE)
#define IOCTL_XINPUT_SET_GAMEPAD_STATE        0x8000A010  // CTL_CODE(0x8000,0x804,BUFFERED,WRITE)
#define IOCTL_XINPUT_WAIT_FOR_GUIDE_BUTTON    0x8000A014
#define IOCTL_XINPUT_GET_BATTERY_INFORMATION  0x8000E018
#define IOCTL_XINPUT_POWER_DOWN_DEVICE        0x8000A01C
#define IOCTL_XINPUT_GET_AUDIO_INFORMATION    0x8000E020
#define IOCTL_XINPUT_GET_BASE_BUS_INFORMATION 0x8000E3FC

#define XUSB_VERSION_1_0 ((USHORT)0x0100)
#define XUSB_VERSION_1_1 ((USHORT)0x0101)
#define XUSB_VERSION_1_2 ((USHORT)0x0102)

// XInput gamepad button bits (subset; A is all we toggle here)
#define XINPUT_GAMEPAD_A 0x1000

// Full button mask without the Guide bit (0x0400), for the capabilities report
#define XINPUT_BUTTON_MASK_WITHOUT_GUIDE 0xF3FF

#define XINPUT_DEVTYPE_GAMEPAD    0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD 0x01
#define XINPUT_CAPS_VOICE_SUPPORTED 0x0004

// LED pattern XINPUT_LED_1 → maps to XInput user port 0
#define XINPUT_LED_1 6

// Identity we advertise (a generic wired Xbox 360 pad — the canonical XInput device)
#define XUSB_SPIKE_VID 0x045E
#define XUSB_SPIKE_PID 0x028E

// Flip the emulated A button every this-many GET_GAMEPAD_STATE polls, so a fixed-rate
// XInputGetState() poll visibly sees the button (and packet number) change over time.
#define XUSB_SPIKE_TOGGLE_PERIOD 20

#include <pshpack1.h>

typedef struct _XUSB_IN_BASE_REQUEST
{
	USHORT XUSBVersion;
	UCHAR  DeviceIndex;
} XUSB_IN_BASE_REQUEST;

typedef struct _XUSB_OUT_DEVICE_INFO
{
	USHORT XUSBVersion;
	UCHAR  DeviceCount;   // controllers present on this interface
	UCHAR  Unk1;
	UCHAR  Flags;         // bit 0x80 tells the client to skip this device — must be clear
	UCHAR  Unk3;
	USHORT Unk4;
	USHORT VendorId;
	USHORT ProductId;
} XUSB_OUT_DEVICE_INFO;

typedef struct _XUSB_OUT_LED
{
	USHORT XUSBVersion;
	UCHAR  LEDState;
} XUSB_OUT_LED;

typedef struct _XUSB_OUT_CAPS_0101
{
	USHORT XUSBVersion;
	UCHAR  Type;
	UCHAR  SubType;
	USHORT wButtons;
	UCHAR  bLeftTrigger;
	UCHAR  bRightTrigger;
	SHORT  sThumbLX;
	SHORT  sThumbLY;
	SHORT  sThumbRX;
	SHORT  sThumbRY;
	UCHAR  Unk16;
	UCHAR  Unk17;
	UCHAR  Unk18;
	UCHAR  Unk19;
	UCHAR  Unk20;
	UCHAR  Unk21;
	UCHAR  bLeftMotorSpeed;
	UCHAR  bRightMotorSpeed;
} XUSB_OUT_CAPS_0101;

typedef struct _XUSB_OUT_STATE_0101
{
	USHORT XUSBVersion;
	UCHAR  Status;        // 1 == connected/active
	UCHAR  Unk2;
	UCHAR  InputId;
	ULONG  dwPacketNumber;
	UCHAR  Unk4;
	UCHAR  Unk5;
	USHORT wButtons;
	UCHAR  bLeftTrigger;
	UCHAR  bRightTrigger;
	SHORT  sThumbLX;
	SHORT  sThumbLY;
	SHORT  sThumbRX;
	SHORT  sThumbRY;
	UCHAR  Unk6;
	UCHAR  Unk7;
	UCHAR  Unk8;
	UCHAR  Unk9;
	UCHAR  Unk10;
	UCHAR  bExtraButtons;
} XUSB_OUT_STATE_0101;

#include <poppack.h>

// Per-device state for the spike
typedef struct _XUSB_SPIKE_CONTEXT
{
	volatile LONG PollCount;
} XUSB_SPIKE_CONTEXT, *PXUSB_SPIKE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_SPIKE_CONTEXT, XusbSpikeGetContext)

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL XusbSpikeEvtIoDeviceControl;

// ---------------------------------------------------------------------------

BOOLEAN XusbSpikeIsSpikeDevice(_In_ PWDFDEVICE_INIT DeviceInit)
{
	WCHAR ids[512];
	ULONG resultLength = 0;

	// Hardware IDs come back as a REG_MULTI_SZ; a substring search over the whole
	// buffer is enough to recognise our test id ROOT\DUOXUSBTEST.
	RtlZeroMemory(ids, sizeof(ids));

	NTSTATUS status = WdfFdoInitQueryProperty(
		DeviceInit,
		DevicePropertyHardwareID,
		sizeof(ids) - sizeof(WCHAR),   // leave room for a terminator
		(PVOID)ids,
		&resultLength);

	if (!NT_SUCCESS(status))
	{
		return FALSE;
	}

	if (wcsstr(ids, L"DUOXUSBTEST") != NULL)
	{
		return TRUE;
	}

	return FALSE;
}

NTSTATUS XusbSpikeCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
	WDF_OBJECT_ATTRIBUTES attributes;
	WDFDEVICE device;
	WDF_IO_QUEUE_CONFIG queueConfig;
	NTSTATUS status;

	// A plain function driver (NOT a filter): this device owns its devnode and is
	// the target of the XUSB IOCTLs.
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, XUSB_SPIKE_CONTEXT);

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	// Advertise the XUSB device interface so XInput/GameInput can find us.
	status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_XUSB, NULL);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	// Default parallel queue that receives all DeviceIoControl traffic.
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
	queueConfig.EvtIoDeviceControl = XusbSpikeEvtIoDeviceControl;

	status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return STATUS_SUCCESS;
}

VOID XusbSpikeEvtIoDeviceControl(
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

	UNREFERENCED_PARAMETER(InputBufferLength);

	WDFDEVICE device = WdfIoQueueGetDevice(Queue);
	PXUSB_SPIKE_CONTEXT ctx = XusbSpikeGetContext(device);

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
			info.DeviceCount = 1;      // one controller on this interface
			info.Flags = 0;            // 0x80 clear -> do not skip
			info.VendorId = XUSB_SPIKE_VID;
			info.ProductId = XUSB_SPIKE_PID;
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
			led.LEDState = XINPUT_LED_1;   // -> user port 0
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
			caps.sThumbLX = (SHORT)-64;   // 0xFFC0: full range, low 6 bits unreported
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
			LONG count = InterlockedIncrement(&ctx->PollCount);
			BOOLEAN aDown = (((count / XUSB_SPIKE_TOGGLE_PERIOD) & 1) != 0);

			XUSB_OUT_STATE_0101 state;
			RtlZeroMemory(&state, sizeof(state));
			state.XUSBVersion = XUSB_VERSION_1_1;
			state.Status = 1;                  // connected
			state.dwPacketNumber = (ULONG)count;
			state.wButtons = aDown ? XINPUT_GAMEPAD_A : 0;
			RtlCopyMemory(outBuf, &state, sizeof(state));
			bytesReturned = sizeof(state);
		}
		break;
	}

	case IOCTL_XINPUT_SET_GAMEPAD_STATE:
		// LED assignment / rumble writes: accept and ignore for the spike.
		status = STATUS_SUCCESS;
		bytesReturned = 0;
		break;

	default:
		// Battery/audio/guide/bus-info probes are optional for XInputGetState;
		// failing them cleanly is fine and keeps the surface minimal.
		status = STATUS_INVALID_DEVICE_REQUEST;
		bytesReturned = 0;
		break;
	}

	// Never claim more bytes than the caller's buffer holds.
	if (bytesReturned > OutputBufferLength)
	{
		bytesReturned = OutputBufferLength;
	}

	WdfRequestCompleteWithInformation(Request, status, (ULONG_PTR)bytesReturned);
}
