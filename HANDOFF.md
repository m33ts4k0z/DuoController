# DuoController XUSB / XInput Work — Session Handoff

**Date:** 2026-07-15
**Build machine:** DIMI-9950X3D (Windows 11 Pro 25H2, build 26200), user `ff_be`
**Goal:** Make the emulated Xbox Elite controller expose a *real* XInput (XUSB) device from the user-mode (UMDF) DuoController driver, so it works natively in Playnite (and all XInput games) with no environment-variable workaround.

---

## TL;DR — current status

- **Root cause of the original bug is fully understood** (see below).
- **A working interim fix exists**: machine-wide env var `SDL_JOYSTICK_HIDAPI=0` makes Playnite (SDL3) read the pad via DirectInput. Ships today.
- **The proper fix (native XUSB in the driver) is PROVEN VIABLE**: a canned test device (`ROOT\DUOXUSBTEST`, the "spike") was enumerated and read by XInput — slot 0 active, `state=0`. So genuine XInput needs **no kernel driver and no attestation certificate**; a UMDF driver can serve XUSB on 25H2.
- **The real feature is implemented and builds** (see files below), and **devgen-created XUSB Xbox devices work** (TESTXBOX / TESTISO / TESTX2 all Status OK, XInput slot 0 responds).
- **OPEN ISSUE:** when **Sunshine** creates the Xbox pad, the device fails to start. Latest observed failure (before build #9): `0xc0000182` (STATUS_DEVICE_CONFIGURATION_ERROR) / Device Manager Code 31, in a create-retry loop. Build #9 removed session isolation to address this; the user then reported "3 devices with yellow triangles, no longer looping" but **we have NOT yet captured build #9's actual problem code** — that is the immediate next diagnostic.

---

## The original problem & root cause

Symptom: the virtual Xbox One Elite (custom Sunshine build + DuoController UMDF driver on a Moonlight host) worked in games but **not in Playnite**, and XInput was empty everywhere.

1. **DuoController's Xbox path only exposed a plain HID gamepad** (VID_045E / PID_02FF) and delegated "XInput" to the inbox `xinputhid.sys` via the INF. That delegation is **inert for a UMDF virtual pad** — every `DevicePropertyFlags` mode was tested on the host (0xE GIP, 0x6 generic, 0x1 BusDevice); all left `XInputGetState` returning 1167 (empty) on all 4 slots. Games worked via GameInput/raw-HID, never via XInput.
2. **Why "Playnite only":** the Playnite fork ships `SDL2.dll` as **sdl2-compat over SDL3 3.4.4**. SDL3 routes any known Xbox VID/PID (02FF is in its `controller_list.h`) to HIDAPI first (stalls on the GIP handshake the driver doesn't speak) or to the dead XInput backend. Classic SDL2 used DirectInput and worked — hence "worked once, then stopped" (also entangled with the user swapping SDL2.dll versions and `SDL_JOYSTICK_HIDAPI` env-var scope).

Key repos referenced:
- **Playnite fork:** `F:\source\repos\Playnite` (10.56 + .NET 10 retarget). Ships SDL2.dll (sdl2-compat) + SDL3 3.4.4.
- **XUSB IOCTL protocol reference:** `Nemirtingas/OpenXinput` (a faithful reimplementation of Microsoft's xinput1_x.dll — the *client* side). **ViGEmBus is NOT a model** for the responder: it emulates at the USB layer and lets Microsoft's `xusb22.sys` answer the IOCTLs.

---

## The two fixes

### Interim (works today)
Set machine-wide `SDL_JOYSTICK_HIDAPI=0` on the host, keep the HID child id as `...&DUOCONTROLLER` (NO `IG_`). SDL then reads the pad via DirectInput. Correct config for the *old* HID-based driver.

### Proper (the XUSB work — in progress)
Implement the XUSB device interface directly in DuoController: register `GUID_DEVINTERFACE_XUSB` `{EC87F1E3-C13B-4100-B5F7-8B84D54260CB}` and answer the XUSB IOCTL protocol. The Xbox controller becomes an **XUSB-only function driver** (not a HID filter), exactly like real hardware — which also makes the SDL/HIDAPI conflict disappear.

---

## What was implemented (source changes in `F:\source\repos\DuoController\DuoController\`)

- **`Public.h`** — added `DEFINE_GUID(GUID_DEVINTERFACE_XUSB, ...)`.
- **`XusbSpike.h` / `XusbSpike.c`** — standalone canned XUSB test device (`ROOT\DUOXUSBTEST`). Proves UMDF-served XUSB works; toggles the A button so a poll shows movement. Created via `devgen`, no Sunshine needed.
- **`Xusb.h` / `Xusb.c`** — the REAL XUSB Xbox device:
  - `XusbIsXboxDevice()` — routes hardware ids containing `PID_02FF`.
  - `XusbCreateDevice()` — plain UMDF **function driver** (no `WdfFdoInitSetFilter`); registers the XUSB interface; reuses `DEVICE_CONTEXT` + the existing shared-memory server; creates the default IOCTL queue + a manual queue (the shared-mem worker resolves the device context through it).
  - `XusbEvtIoDeviceControl()` — answers `GET_INFORMATION / GET_LED_STATE / GET_CAPABILITIES / GET_GAMEPAD_STATE`; `SET_GAMEPAD_STATE` accepted (rumble not yet forwarded).
  - `XusbTranslateReport()` — converts the live 64-byte Xbox HID report (written by the client via shared memory into `deviceContext->InputReport`) into XUSB gamepad state. Sticks re-centered to signed range with Y inverted; triggers 10-bit→8-bit; D-pad/buttons remapped; **paddles dropped** (XInput has no paddle bits, like a real Elite through XInput).
- **`Driver.c`** — `DuoControllerEvtDeviceAdd` branches: `DUOXUSBTEST`→spike, `PID_02FF`→`XusbCreateDevice`, else the unchanged HID path (DualSense/DS4 untouched).
- **`Device.h`** — added `ULONG XusbPacketNumber; UCHAR XusbLastReport[XB1_REPORT_SIZE];` to `DEVICE_CONTEXT` (packet-change detection).
- **`DuoController.c` (client library)** — `CreateXboxController` now passes `NULL` for the HID child-id out-params (no HID child anymore; also skips the old 5s child-enumeration wait). Seed still `VID_045E&PID_02FF&DUOCONTROLLER` (becomes the root instance id, keys the shared memory).
- **`DuoController.inf`** — Xbox section (`DuoController_Install_Xbox`) rewritten from HID+xinputhid to **plain UMDF XUSB**; spike section (`DuoController_Install_Xusb`) added. Service fixes below.
- **`DuoController.vcxproj`** — added `Xusb.c/.h`, `XusbSpike.c/.h`; `<TimeStamp>` bumped to **1.5.11.0**.

---

## Build & deploy

**MSBuild:** `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
**WDK (NuGet, x64-only):** `F:\source\repos\DuoController\packages\Microsoft.Windows.WDK.x64.10.0.28000.1839`

```powershell
# The x64-only WDK NuGet has NO x86-hosted tools, so:
$env:PATH = "F:\source\repos\DuoController\packages\Microsoft.Windows.WDK.x64.10.0.28000.1839\c\bin\10.0.28000.0\x64;$env:PATH"
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "F:\source\repos\DuoController\DuoController\DuoController.vcxproj" `
  /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m
```
- The trailing `INF verification exception: Unable to load DLL 'x86\InfVerif.dll'` is **non-fatal** — the package still builds and the catalog still signs.
- Compiler warnings ARE errors (e.g. C4310 truncation); prefast /analyze warnings (C6386) are not.
- **Output package:** `F:\source\repos\DuoController\DuoController\x64\Release\DuoController\` (dll, inf, cat). Auto-signed with WDK test cert **`WDKTestCert ff_be`**, thumbprint `3D3316256A7600A0EC4787ABB305757A999D4AEF` (in `CurrentUser\My` on the build machine).
- Validate the STAMPED output inf (not the source — source has unresolved `$ARCH$`): `...\packages\...\c\tools\10.0.28000.0\x64\infverif.exe /v /w <inf>`.

**Cert trust on any test machine (once):**
```powershell
Import-Certificate -FilePath .\DuoController.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\DuoController.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

**Sunshine:** service `SunshineService` → `F:\Applications\Sunshine\tools\sunshinesvc.exe`.
⚠ **UNRESOLVED:** we never located where Sunshine loads `DuoController.dll` from — it is NOT in `F:\Applications\Sunshine\` nor `C:\Program Files\Duo\DuoController` (empty on this machine). The client force-installs its bundled INF via `DiInstallDriverW(DIIRFLAG_FORCE_INF)` on each controller creation, so **whatever folder Sunshine loads the DLL from must be updated** with the new build. **Find this path before deploying.** (Check Sunshine's config / how the custom build was set up.)

**pnputil dedups by DriverVer** — re-adding a same-version package reports "Already exists / Added: 0". Bump `<TimeStamp>` or delete the old `oemNN.inf` first. Helper scripts live in the package folder: `Redeploy.ps1` (stops Sunshine, removes all DuoController devnodes incl. `ROOT\DEVGEN\*` zombies, deletes stale packages, stages the new one), `Test-Xusb.ps1` (XInput poll), `Install-XusbSpike.ps1` / `Remove-XusbSpike.ps1`, plus `devgen.exe` and `DuoController.cer`.

---

## Test tools & procedures

- **`Test-Xusb.ps1`** — polls `xinput1_4.dll` and prints 4 slots. Success = slot 0 `state=0` (not `--`), with `btn`/`pkt` changing as buttons are pressed. **Must run in the same session as the device** (though build #9 removed isolation, so the XUSB device should now be global).
- **`devgen.exe`** — create the real Xbox XUSB device WITHOUT Sunshine (no retry loop):
  `.\devgen.exe /add /bus ROOT /instanceid TESTXBOX /hardwareid "ROOT\VID_045E&PID_02FF&IG_00"` → check `Get-PnpDevice -PresentOnly | ? InstanceId -like "ROOT\DEVGEN*"`. This exercises `XusbCreateDevice` + shared memory. **This path works (Status OK).**
- **`DuoControllerSample.exe`** (BUILT: `F:\source\repos\DuoController\DuoControllerSample\x64\Release\DuoControllerSample.exe`) — loads `DuoController\DuoController.dll` (relative path!), calls `DuoController_Initialize` + `DuoController_CreateController(Xbox,...)`, prints HRESULTs, then reads input from stdin (`A 1` presses A, `LeftStickHorizontal 65535`, `reset`, `exit`). **This is the tool to reproduce Sunshine's exact CreateController path in a controlled way — run it next.**

---

## Build history & bugs fixed (iterations)

1. **IG_ experiments** — changed the HID child id to include `IG_00` for SDL's XInput heuristic; tested all `xinputhid` `DevicePropertyFlags` modes on the host. **All failed (1167).** Concluded xinputhid cannot serve a UMDF virtual pad. Reverted; adopted `SDL_JOYSTICK_HIDAPI=0` interim.
2. **Spike** — proved UMDF XUSB works (slot 0 active). The key positive result.
3. **Real feature** (`Xusb.c`) built.
4. **INF service association** — infverif `ERROR(1296)` / pnputil "service installation section invalid": a UMDF-only device needs its `[X.NT.Services]` to mark WUDFRd as the associated service via flag **`0x000001fa`** (SPSVCINST_ASSOCSERVICE), not `0x1f8`.
5. **Service binary path** — `ServiceBinary = %13%\WUDFRd.sys` was wrong (`%13%` = package dir; WUDFRd.sys is inbox). Caused "binary not present" → **Code 31 create-loop**. Fixed to **`%12%\WUDFRd.sys`** (System32\drivers). Also fixed `mshidumdf` the same way. (infverif `ERROR(2084)` about these remains — it's non-fatal, present in the working HID sections too.)
6. **Inverted return check** — `CreateSharedMemoryServer` returns **0 on success, >0 on failure**. `XusbCreateDevice` had `if (== 0) return STATUS_UNSUCCESSFUL` → aborted on success with `0xc0000001` (CM_PROB_FAILED_ADD). Fixed to `if (> 0)`.
7. **Session isolation removed** (build #9) — the XUSB path no longer assigns `DEVPKEY_Device_SessionId`. Rationale: XInput is a **global, non-session API** (same 4 slots in every session); ViGEmBus doesn't isolate either, and its pads still reach every session because device interfaces are globally visible.

---

## Ruled out

- **Cert trust** — both `LocalMachine\Root` and `TrustedPublisher` have the WDK test cert.
- **INF service-section validity** — fixed (0x1fa + %12%); infverif's remaining 2084 matches the working HID sections.
- **Inverted shared-memory check** — fixed.
- **Session isolation as the *simple* cause** — `TESTX2` (devgen device isolated to a *different* target session) was **Status OK**, so cross-*target* isolation via devgen is fine. The untested variable is Sunshine installing from **session 0 (service)** — which build #9 sidesteps by removing isolation entirely.

---

## OPEN ISSUE & immediate next step

**Question:** why does the **Sunshine-created** Xbox device fail to start while **devgen-created** ones succeed, on the same build/INF/section? devgen always installs from the interactive session; Sunshine (service) installs from session 0 — that is the one variable devgen cannot reproduce.

**Next diagnostics (do these first on the target machine):**
1. Capture build #9's actual failure — connect Moonlight, then:
   ```powershell
   Get-PnpDevice -PresentOnly | ? InstanceId -like "ROOT\VID_045E&PID_02FF*" | % {
     $ps  = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_ProblemStatus'  -EA SilentlyContinue).Data
     $drv = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_DriverVersion'   -EA SilentlyContinue).Data
     "{0}  problem={1}  ntstatus={2}  driverver={3}" -f $_.InstanceId,$_.Problem,$ps,$drv }
   ```
   Confirm `driverver=1.5.11.0` (else it's a zombie on an old package). Note the ntstatus (still `0xc0000182`? something new?).
   Also: `Select-String C:\Windows\INF\setupapi.dev.log -Pattern "DUOCONTROLLER|CM_PROB|0xc000" | Select -Last 20`
2. **Run `DuoControllerSample.exe`** interactively (streamed session) — it hits the exact client CreateController path. If it *succeeds* interactively while Sunshine fails, the culprit is confirmed as **Sunshine's session-0 service install context**; if it *fails* too, the issue is in the create path itself. Either way you get an HRESULT.
3. If confirmed session-0-install: options are (a) have Sunshine create the device from the user session, or (b) investigate what a session-0-installed UMDF function driver needs to start (WUDF host launch in the target session).

**Fallback always available:** revert to the HID build + `SDL_JOYSTICK_HIDAPI=0` machine-wide — fully working today.

**Known remaining gaps (once it starts):** rumble not forwarded (SET_GAMEPAD_STATE accepted but ignored); paddles dropped (XInput limitation); stick/button mapping may need a one-line tweak in `XusbTranslateReport` if any axis is inverted/swapped.

---

## Accident to be aware of

During isolation testing I had the user run `New-Item -Path "HKLM:\SOFTWARE\Duo" -Force` — the `-Force` on the existing key wiped Sunshine's `SOFTWARE\Duo\Instances` subkey and broke Sunshine startup ("The SOFTWARE\Duo\Instances registry key couldn't be opened"). Recovered by recreating the key WITHOUT `-Force`:
```powershell
if (-not (Test-Path "HKLM:\SOFTWARE\Duo\Instances")) { New-Item -Path "HKLM:\SOFTWARE\Duo\Instances" | Out-Null }
```
Never use `-Force` on `HKLM:\SOFTWARE\Duo` — Sunshine stores state under it.

---

## Memory files (on the build machine)
`C:\Users\ff_be\.claude\projects\F--source-repos-Aniki-ReMake\memory\` — `duocontroller-xinput.md`, `duocontroller-build.md`.
