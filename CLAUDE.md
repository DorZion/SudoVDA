# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

SudoVDA is a Windows Indirect Display Driver (UMDF / IddCx class extension) that exposes synthetic monitors up to 8K / 500 Hz, with HDR on Windows 11 24H2. The project is Windows-only; the macOS dev environment here cannot compile it — build must happen on Windows with Visual Studio and the WDK.

## Build

- Open `Virtual Display Driver (HDR)/SudoVDA.sln` in Visual Studio, or:
  - `msbuild "Virtual Display Driver (HDR)/SudoVDA.sln" /p:Configuration=Release /p:Platform=x64`
- Configurations: `Debug|Release` × `x64|ARM64`.
- Toolset: `WindowsUserModeDriver10.0`, UMDF 2.25, IddCx 1.10 (`IDDCX_MINIMUM_VERSION_REQUIRED=4`). Requires the WDK matching these versions.
- Output: `SudoVDA.dll` + `SudoVDA.inf` + `sudovda.cat` (installed to `%WinDir%\System32\drivers\UMDF\`). The driver is **test-signed** — install on a test-signing-enabled machine, or re-sign with a production WHQL cert. Install via `pnputil /add-driver SudoVDA.inf /install` (or devcon). Hardware ID: `root\sudomaker\sudovda` (see `SUVDA_HARDWARE_ID`).
- No test suite or linter is wired up; code analysis (`RunCodeAnalysis=true`) and PREfast run during MSBuild.

## Architecture

### Layout
- `Common/Include/sudovda-ioctl.h` — the **public contract** with client apps. Defines IOCTL codes, struct layouts, and `VDAProtocolVersion`. Bump the version whenever the IOCTL surface changes; clients check it via `IOCTL_GET_PROTOCOL_VERSION` before talking to the driver.
- `Common/Include/AdapterOption.h` — header-only DXGI adapter enumeration / selection (picks the GPU with the largest VRAM by default, or matches `gpuName` from registry).
- `Virtual Display Driver (HDR)/SudoVDA/Driver.cpp` — the entire driver (~2100 lines, monolithic by design). `Driver.h` declares the three core types: `Direct3DDevice`, `SwapChainProcessor`, `IndirectMonitorContext`, `IndirectDeviceContext`.
- `Virtual Display Driver (HDR)/SudoVDA/edid.h` — base EDID blob + `generate_edid()` that stamps per-monitor model code, serial number, and friendly name.

### IddCx callback dispatch
`SudoVDADeviceAdd` inspects `IDD_IS_FIELD_AVAILABLE(..., EvtIddCxAdapterQueryTargetInfo)` at runtime and wires **two alternative sets** of IddCx callbacks:
- Modern (HDR-capable, Win11 24H2): `...ParseMonitorDescription2`, `...MonitorQueryTargetModes2`, `...AdapterCommitModes2`, plus HDR metadata and gamma ramp hooks. `isHDRSupported` is set to true.
- Legacy (Win10 / older Win11): the non-`2` variants. No HDR.

When editing mode/commit logic, update **both** branches or the driver will regress on one OS family.

### Monitor lifecycle state machine
The driver tracks monitors via `std::list<IndirectMonitorContext*> monitorCtxList` guarded by `monitorListOp` (recursive mutex), plus a `std::set<size_t> freeConnectorSlots` pool. Each context carries `isConnected`, a `connectorId`, a `monitorGuid`, and `m_Monitor` (the IddCx handle). There are four distinct transitions, all in `SudoVDAIoDeviceControl`:
- `ADD` — creates a new monitor, or reconnects an existing GUID (including registry-persisted entries with no live IddCx handle).
- `REMOVE` — deletes permanently. **The only path that returns `connectorId` to `freeConnectorSlots`** and deletes the registry entry.
- `DISCONNECT` — `IddCxMonitorDeparture` only. Keeps context, handle, and connector slot so a later `RECONNECT` resurrects the exact same Windows device identity (preserving ICC profiles, display numbering, etc.).
- `RECONNECT` — prefers to re-arrive on the existing `IDDCX_MONITOR` handle; if Windows has destroyed it (stale handle / cross-reboot), creates a new monitor on the same preserved `connectorId` and calls `IddCxMonitorArrival`.

`MonitorCleanupCallback` deliberately **does not** return the connector slot when Windows destroys the IddCx object — the slot stays reserved to the GUID. This is load-bearing for identity preservation; don't "fix" it.

### Persistence
Per-display state lives at `HKLM\SOFTWARE\SudoMaker\SudoVDA\Displays\{guid}` (Width, Height, VSync, ConnectorIndex, SerialStr, DeviceName). `LoadPersistedDisplays()` runs at adapter init and rebuilds `IndirectMonitorContext` entries in a disconnected state (no live IddCx handle, connector slot pre-reserved); a later `ADD` or `RECONNECT` with the matching GUID brings them online. `SaveDisplayToRegistry` is called on successful `ADD`; `RemoveDisplayFromRegistry` only on `REMOVE`.

### Mode reporting
`s_DefaultModes[]` (the large table at the top of Driver.cpp) + `mode_scale_factors[]` are combined with each monitor's `preferredMode` (set from the `ADD` IOCTL params) to produce the list returned by `SudoVDAMonitorQueryModes` / `...QueryModes2`. The preferred mode gets both a 1:1 entry and scaled variants. All refresh rates in this codebase use **millihertz** (e.g. `60000` = 60 Hz, `59940` = 59.94 Hz). Use a `1000` denominator convention (see commit `a21501f`).

### Watchdog
A background thread in `RunWatchdog()` decrements `watchdogCountdown` each second. Every IOCTL **except** `IOCTL_GET_WATCHDOG` resets it to `watchdogTimeout`. If a client stops pinging, the watchdog calls `DisconnectAllMonitors(false)` — contexts are retained so a reconnect can restore identity. Clients are expected to poll `IOCTL_GET_WATCHDOG` or send periodic `IOCTL_DRIVER_PING`s.

### Runtime configuration
Driver reads `HKLM\SOFTWARE\SudoMaker\SudoVDA` on load (see `LoadSettings()`): `gpuName`, `maxMonitors`, `watchdog`, `sdrBits` (8/10), `hdrBits` (10/12), plus `testMode`. Changes require a driver reload or reboot. See the root README for user-facing docs on these keys.

## Conventions

- Protocol changes (new/changed IOCTL, struct field reordering) **must** bump `VDAProtocolVersion` in `sudovda-ioctl.h`.
- Never return a connector slot to `freeConnectorSlots` outside `IOCTL_REMOVE_VIRTUAL_DISPLAY` or the `deleteContexts=true` path of `DisconnectAllMonitors` — doing so breaks device-identity preservation across reconnects.
- Contexts are added to `monitorCtxList` **only after** `IddCxMonitorArrival` succeeds (see the `CreateMonitor` flow) — Windows callbacks can fire as soon as the monitor object exists and will crash on partially-initialized state.
- The `MonitorContainerId` passed to IddCx **is** the `MonitorGuid` supplied by the client; it doubles as the persistence key.
