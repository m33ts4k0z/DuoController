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
// XUSB SPIKE
//
// A standalone, self-contained proof-of-concept device that registers the XUSB
// device interface (GUID_DEVINTERFACE_XUSB) and answers the XUSB IOCTL protocol
// with canned data, so we can answer one question empirically:
//
//     On this Windows build, will XInput (xinput1_x.dll, and on Win11 24H2+ the
//     GameInput service that backs it) enumerate and read an XUSB interface that
//     is served by a *user-mode* (UMDF) driver rather than Microsoft's xusb22.sys?
//
// It is NOT wired to the shared-memory input path — it reports a controller with
// the A button toggling on a call counter, so a plain XInputGetState() poll shows
// an unmistakable "connected + buttons changing" signal (or all-1167 if XInput
// refuses UMDF-provided XUSB devices, which is itself the answer).
//
// The device is created by a dedicated INF install section bound to the hardware
// id ROOT\DUOXUSBTEST and can be materialised with `devgen` — no Sunshine/Moonlight
// session and no HID plumbing involved, so it isolates exactly the variable above.
// ---------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include <wdf.h>

EXTERN_C_START

// Returns TRUE if the device being added (per its hardware id) is the XUSB spike
// test device, in which case DriverEntry's EvtDeviceAdd routes to XusbSpikeCreateDevice
// instead of the normal HID controller path.
BOOLEAN XusbSpikeIsSpikeDevice(_In_ PWDFDEVICE_INIT DeviceInit);

// Creates the XUSB spike function device: a non-filter FDO that exposes the XUSB
// device interface and services the XUSB IOCTLs from its default queue.
NTSTATUS XusbSpikeCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit);

EXTERN_C_END
