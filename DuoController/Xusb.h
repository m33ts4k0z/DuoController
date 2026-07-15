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

// ---------------------------------------------------------------------------
// XUSB (real) Xbox device
//
// Exposes the emulated Xbox controller as an XUSB device — the interface that
// XInput (xinput1_x.dll, and the GameInput service that backs it on Win11 24H2+)
// enumerates — instead of a plain HID gamepad. This makes the pad behaviourally
// identical to real Xbox hardware: XInputGetState reads it natively, and the
// SDL/HIDAPI "Xbox VID/PID but not really XInput" conflict disappears entirely.
//
// The device reuses DEVICE_CONTEXT and the existing shared-memory input server,
// so the client library keeps sending the same 64-byte Xbox HID report via
// DuoController_SendReport; this device just translates that live report into the
// XUSB gamepad-state protocol on demand.
//
// Viability of a *user-mode* (UMDF) XUSB provider on Win11 25H2 was confirmed by
// the standalone XusbSpike test device before this was written.
// ---------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include <wdf.h>

EXTERN_C_START

// XUSB IOCTL control codes (from the XInput client that issues them; see Xusb.c).
#define IOCTL_XINPUT_GET_INFORMATION          0x80006000
#define IOCTL_XINPUT_GET_CAPABILITIES         0x8000E004
#define IOCTL_XINPUT_GET_LED_STATE            0x8000E008
#define IOCTL_XINPUT_GET_GAMEPAD_STATE        0x8000E00C
#define IOCTL_XINPUT_SET_GAMEPAD_STATE        0x8000A010
#define IOCTL_XINPUT_WAIT_FOR_GUIDE_BUTTON    0x8000A014
#define IOCTL_XINPUT_GET_BATTERY_INFORMATION  0x8000E018
#define IOCTL_XINPUT_POWER_DOWN_DEVICE        0x8000A01C
#define IOCTL_XINPUT_GET_AUDIO_INFORMATION    0x8000E020
#define IOCTL_XINPUT_GET_BASE_BUS_INFORMATION 0x8000E3FC

#define XUSB_VERSION_1_1 ((USHORT)0x0101)

// XInput gamepad button bits
#define XI_DPAD_UP        0x0001
#define XI_DPAD_DOWN      0x0002
#define XI_DPAD_LEFT      0x0004
#define XI_DPAD_RIGHT     0x0008
#define XI_START          0x0010
#define XI_BACK           0x0020
#define XI_LEFT_THUMB     0x0040
#define XI_RIGHT_THUMB    0x0080
#define XI_LEFT_SHOULDER  0x0100
#define XI_RIGHT_SHOULDER 0x0200
#define XI_GUIDE          0x0400
#define XI_A              0x1000
#define XI_B              0x2000
#define XI_X              0x4000
#define XI_Y              0x8000

#define XINPUT_BUTTON_MASK_WITHOUT_GUIDE 0xF3FF
#define XINPUT_DEVTYPE_GAMEPAD    0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD 0x01
#define XINPUT_CAPS_VOICE_SUPPORTED 0x0004
#define XINPUT_LED_1 6   // -> XInput user port 0

// Identity of the emulated pad, as reported to XInput. 045E:028E == wired Xbox 360
// pad, the canonical XInput device that every title recognises.
#define XUSB_XBOX_VID 0x045E
#define XUSB_XBOX_PID 0x028E

#include <pshpack1.h>

typedef struct _XUSB_OUT_DEVICE_INFO
{
	USHORT XUSBVersion;
	UCHAR  DeviceCount;
	UCHAR  Unk1;
	UCHAR  Flags;        // 0x80 clear -> do not skip
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
	UCHAR  Status;       // 1 == connected
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

// Client-side SET_GAMEPAD_STATE payload (LED assignment / rumble). Matches the
// InSetState_t the XInput client sends.
typedef struct _XUSB_IN_SET_STATE
{
	UCHAR DeviceIndex;
	UCHAR LedState;
	UCHAR LeftMotorSpeed;
	UCHAR RightMotorSpeed;
	UCHAR Flags;          // 0x01 LED, 0x02 vibration
} XUSB_IN_SET_STATE;

#include <poppack.h>

// Returns TRUE if the device being added is the real Xbox controller that should
// be exposed as XUSB (hardware id carries the Xbox VID/PID marker).
BOOLEAN XusbIsXboxDevice(_In_ PWDFDEVICE_INIT DeviceInit);

// Creates the XUSB Xbox function device (non-filter): registers the XUSB interface,
// stands up the shared-memory input server, and services the XUSB IOCTL protocol.
NTSTATUS XusbCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit);

EXTERN_C_END
