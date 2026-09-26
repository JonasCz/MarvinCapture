# Windows: switching the 500-USB to WinUSB for development

This machine already has the vendor's **Pinnacle Video Driver** MSI installed
(`Pinnacle_Video_Driver_64bit.msi`, in the repo root — uninstall entry
`Pinnacle Video Driver 12.1.0.029`, product GUID `{6DE721A5-5E89-4...}`, see
`driver-status.ps1` output). It owns several driver packages. Only one of them
touches the actual capture device; the rest are for other Pinnacle hardware
(Dazzle DVC10x, MovieBox) that also came in the same package. This note is
concise and command-first; run `tools/windows/driver-status.ps1` any time to
re-check the state below, it is entirely read-only.

## 1. What's installed, and what it does

| Published INF | Original name | Matches | What it is |
|---|---|---|---|
| **oem74.inf** | `MarvinAVS64.inf` | `USB\VID_2304&PID_0213` (and sibling PIDs `0206`/`0212`/`0223`/`0224` — see below) | **This is the one.** The kernel-mode WDM/AVStream function driver that binds directly to the USB device. Service `PinnacleMarvinAVS`, binary `MarvinAVS64.sys`, class **Media**. |
| oem84.inf | `MarvinBus64.inf` | `root\MarvinBus` (a **software/virtual** device, not the USB hardware ID) | A root-enumerated virtual 1394 bus device, `ROOT\SYSTEM\0001`, "Pinnacle Marvin Bus 64". Service `MarvinBus`. This is presumably created by MarvinAVS64.sys to expose the FPGA's emulated OHCI-1394 controller to the legacy 1394 bus stack (`legacy1394.inf`/AVC stack) so DV/AVC software sees a normal FireWire camera. It is **not bound to the physical USB device** and does not need to be touched for WinUSB to work. |
| oem85.inf | `pclebend64.inf` | (no VID_2304 match found) | Pinnacle Systems, Media class — unrelated helper package from the same installer, not investigated further; leave alone. |
| oem86.inf | `dvcaudio64.inf` | `USB\VID_2304&PID_021A&MI_01` | Dazzle DVC100/103/107 **audio** function — different PID (`021A`), not our device. |
| oem87.inf | `dvcvideo64.inf` | `USB\VID_2304&PID_021A&MI_00` | Dazzle DVC100 **video** function, same different-PID family. Installs upper/lower KS filters (`ScanUSBEMPIA`/`FiltUSBEMPIA`) — again, unrelated device. |

**Sibling PIDs**, all in `oem74.inf` (`MarvinAVS64.inf`, one `[Manufacturer]` section
covers the whole Marvin family — confirms the repo's note about future Marvin
models):

```
USB\VID_2304&PID_0206   Marvin-classic
USB\VID_2304&PID_0212   Marvin-CR
USB\VID_2304&PID_0213   Marvin-Lite   <- the 500-USB, this repo's device
USB\VID_2304&PID_0223   Marvin-510
USB\VID_2304&PID_0224   Marvin-710
```

None of these has an `&MI_` suffix, confirming what `docs/usb-descriptors.md`
found from the descriptors directly: **the device is not composite** — one
configuration, one interface (class `0xFF`, vendor-specific, 4 alt settings),
so the *whole device* binds to `MarvinAVS64.sys`, not a child interface. This
materially simplifies Zadig (§3): there is only one thing to select, not a
composite parent plus function children.

**Service states** (`sc qc` / `sc query`, all read-only):

| Service | Binary | Start type | State (device unplugged) |
|---|---|---|---|
| `PinnacleMarvinAVS` | `MarvinAVS64.sys` | **Demand start** (3) | Stopped |
| `MarvinBus` | `MarvinBus64.sys` | **Demand start** (3) | **Running** (`ROOT\SYSTEM\0001` is root-enumerated and starts at boot regardless of whether the USB device is present) |

Neither is boot- or auto-start, so nothing loads at boot *because of the USB
device* — `PinnacleMarvinAVS` only starts when Windows matches the driver to
the plugged-in device. `MarvinBus` runs independently as a permanent
root-enumerated software device (it doesn't grab the USB device itself, so it
doesn't compete with WinUSB and doesn't need to be stopped or removed).

**No filter drivers on the device.** The USB class key
(`{36FC9E60-...}`) has only `USBPcap` in `UpperFilters` (from Wireshark's USB
capture driver — unrelated, global, harmless). The Media class key has only
the standard `ksthunk` upper filter (normal Windows AVStream plumbing, not
Pinnacle-specific). Nothing sits between the device and its function driver,
so a Zadig replace is a clean single-driver swap.

**No vendor user-mode service or tray app was found running or registered.**
`PinnacleCapture.exe` seen in the process list is this repo's own GUI
(`gui/windows/PinnacleCapture`), not vendor software. So there is nothing at
the user-mode level to fight over the device — only the kernel driver itself,
and only while it's bound.

## 2. Recommended approach for development now

**Use Zadig to replace the driver for `USB\VID_2304&PID_0213` with WinUSB.**
Given the findings above (single non-composite interface, no filters, no
user-mode vendor process, demand-start service), this is a clean, fully
reversible operation:

- Windows keeps the original `oem74.inf` package in the driver store the
  whole time; Zadig only rewrites *which* driver package is selected for this
  specific hardware ID, via a new `oem##.inf` it adds (self-generated,
  signed through libwdi's embedded WinUSB co-installer certificate chain).
- Reverting is one click in Device Manager ("Roll Back Driver" or "Update
  Driver" → let Windows pick, or point it at `oem74.inf` again), or by
  deleting the Zadig-added package with `pnputil /delete-driver oemNN.inf
  /uninstall` once you know its number.
- Nothing needs to be uninstalled, disabled, or deleted from the existing
  Pinnacle install.

### Why not the alternatives

- **(b) Temporarily disabling the vendor driver** (`Disable-PnpDevice` /
  Device Manager "Disable") doesn't get you WinUSB — a disabled device has no
  driver bound at all, and Zadig also can't target a disabled device cleanly.
  It's not a step you need; Zadig's "Replace Driver" does the swap in one
  action.
- **(c) Full removal** (`pnputil /delete-driver oem74.inf /uninstall` and
  uninstalling the Pinnacle Video Driver MSI) is unnecessary and harder to
  reverse — you'd need the original MSI (`Pinnacle_Video_Driver_64bit.msi`,
  present in the repo root, so reverting is still possible) to reinstall.
  Since nothing here fights over the device at the user-mode level (finding
  above), there's no benefit to full removal for development.
- **(d) Vendor software fighting over the device**: not applicable here —
  no vendor tray app or user-mode service was found installed or running.

### What a future end-user installer should do

Realistically:

- **A Microsoft OS 2.0 descriptor / automatic WinUSB binding is not
  possible** — that requires the *device firmware* to advertise the
  `MS_OS_20` descriptor so Windows binds WinUSB automatically with no INF at
  all. This device's firmware is fixed (Pinnacle's original CY7C68013A EEPROM
  image, `docs/startup.md`), and we don't own or redistribute new firmware, so
  this route is closed.
- **The realistic option is a libwdi-based installer** (the library behind
  Zadig): ship a small installer, generated with libwdi's `wdi-simple`/API or
  by exporting a Zadig-generated package (Zadig → Options → "Save extracted
  driver files" or "Create a self-extracting driver installer"), containing a
  device-specific INF that binds `USB\VID_2304&PID_0213` (and, if we want to
  support the sibling PIDs, all five) straight to `winusb.sys`, plus libwdi's
  bundled WinUSB co-installer and certificate. This is exactly what Zadig
  does interactively; a real installer just automates the same steps
  silently and ships our own signed catalog instead of asking the end user to
  run Zadig by hand.
- Such an installer should install to *all* of `PID_0206/0212/0213/0223/0224`
  if the project ever wants to support the other Marvin models, per the
  project's replug/no-hardcoding rule — but each model needs to be verified
  on real hardware first (per the "don't hardcode unit data" memory rule);
  don't ship blind support for units nobody has tested.
- It should **not** touch `MarvinBus64.sys`/`oem84.inf` — that virtual bus
  device is irrelevant to a libusb-based driver and uninstalling it doesn't
  help or hurt.

### Will Windows put the vendor driver back?

Two separate risks, both minor here:

1. **Windows Update pushing a newer driver automatically.** Not expected: the
   vendor package (`oem74.inf`, dated 2007) is an obscure OEM driver, not one
   Windows Update carries a replacement for. If you ever see this happen
   anyway, the standard fix (used by the RTL-SDR community for the same
   Zadig-vs-Windows-Update problem) is the `ExcludeWUDriversInQualityUpdate`
   policy, or simply re-running Zadig's replace — it's idempotent.
2. **A later "Update driver" / "Scan for hardware changes"** could offer to
   reinstall `oem74.inf` since it's still a valid match sitting in the driver
   store. Windows' ranking generally prefers the newer-dated INF when
   hardware IDs tie exactly, so a freshly-Zadig-installed package normally
   wins and stays put across replugs — but if it ever gets reverted, redoing
   the Zadig replace (§3) takes under a minute.

### What `driver-status.ps1` shows even with the device unplugged

Windows remembers devices it has seen before. Running
`tools/windows/driver-status.ps1` on this machine — with the 500-USB
physically unplugged — still prints two `USB\VID_2304&PID_0213\...` instance
IDs with `Status: Unknown` (Windows' term for "not currently present, but
known"). That's expected, not a bug: it's a ghost/ghosted-device entry from a
previous plug-in, and it already shows the bound INF (`oem74.inf`) and INF
section (`MarvinLite.Install`) that confirm §1. Once the device is actually
plugged in, its `Status` becomes `OK` (or `Error` if something's wrong) and
the entry is live. If Zadig has never been run, both entries will show
`Service: PinnacleMarvinAVS`.

## 3. Zadig: exact steps

1. Download Zadig only if/when you're ready to run it (not run automatically
   by this note or by any script here — see the strict rule about not
   downloading or running Zadig without your explicit action):
   <https://zadig.akeo.ie/>.
2. Plug in the Pinnacle 500-USB. Confirm it enumerates as
   `USB\VID_2304&PID_0213` (Device Manager, or
   `tools/windows/driver-status.ps1`).
3. Launch Zadig **as Administrator**.
4. **Options → List All Devices** — check this. Without it, Zadig hides
   devices that already have a driver bound (which this one does, via
   `MarvinAVS64.sys`), so you'd never see it.
5. **Do not** need to touch "Ignore Hubs or Composite Parents" — leave it at
   its default (checked). This device is **not composite** (single
   interface, no `&MI_` suffix — confirmed in §1), so it will show as one
   plain entry, not a composite parent with child interfaces. If you ever see
   *two* entries for it (one greyed out as a composite parent), something
   changed about the device and you should stop and re-check the
   descriptors before proceeding — don't guess.
6. In the device dropdown, find the entry for the Pinnacle device. It will
   most likely show as **"Pinnacle Systems 500-USB Device"** (the
   `MarvinLite.DeviceDesc` string from `oem74.inf`) — confirm the USB ID
   shown under the dropdown reads `2304 0213`. If in doubt, unplug/replug and
   watch which entry disappears/reappears.
7. Target driver dropdown (to the right, next to the green arrow): select
   **WinUSB** (not libusbK — see §4 for why).
8. Optional: **Edit** the driver name field if you want a friendlier label
   than the raw description; this only changes the cosmetic string in the
   generated INF, not the hardware ID it binds to.
9. Click **Replace Driver** (or **Install Driver** if it shows as unbound).
   Confirm the UAC prompt. This takes 10-30 seconds.
10. Verify: `tools/windows/driver-status.ps1` should now show `winusb.sys` /
    `WinUSB` service bound to `USB\VID_2304&PID_0213`, and
    `PinnacleMarvinAVS` should show as not the active driver for that
    instance.

**To revert:** Device Manager → find the device (now listed under
"Universal Serial Bus devices" or "libusb-win32 devices", not "Sound, video
and game controllers") → Driver tab → **Roll Back Driver** if offered, or
**Update Driver → Browse my computer → Let me pick from a list** and choose
"Pinnacle Systems 500-USB Device" from the list of compatible drivers
(`oem74.inf` is still in the driver store, untouched). A replug afterwards
confirms which driver Windows actually bound.

## 4. libusb / transfer-type specifics for this device

**This device has no isochronous endpoints at all** — confirmed in
`docs/usb-descriptors.md`: single vendor-class interface, four alt settings,
every endpoint bulk, 512-byte max packet. This removes the entire
WinUSB-vs-libusbK isochronous question that would otherwise apply:

- WinUSB gained isochronous support in libusb only from **1.0.23** onward
  (Windows 8.1/10+), and even then it's the less mature path — 1-ms frame
  alignment requirements, and real-world reports of
  `LIBUSB_ERROR_NOT_SUPPORTED` on some setups. libusbK has been the more
  reliable choice for isochronous transfers on Windows historically.
- **None of that applies here.** Bulk transfers are supported by every
  libusb Windows backend (WinUSB, libusbK, libusb0) on every Windows version
  libusb still supports. **WinUSB is the right choice** — it's the
  Microsoft-native, no-extra-driver option, and there's no isochronous
  workload to force libusbK.

**libusb version on this machine:** MSYS2 UCRT64 has
`mingw-w64-ucrt-x86_64-libusb 1.0.30-1` installed
(`C:\msys64\ucrt64\bin\libusb-1.0.dll`, file version `1.0.30.12037`,
2026-07-08). This is well past 1.0.23, so WinUSB support (including
isochronous, if it were ever needed for a different Marvin model) is present;
for this device's all-bulk transport, any libusb 1.0.x with a WinUSB backend
works.

**The `LIBUSB_ERROR_NOT_SUPPORTED` case**, for completeness: libusb returns
this on Windows when `libusb_open()`/claim succeeds against a device that
isn't bound to WinUSB/libusbK/libusb0 at all — e.g. if the vendor driver
(`MarvinAVS64.sys`) is still bound. If you see this error after building the
Windows GUI/CLI against this device, it means the Zadig replace (§3) either
didn't take, or reverted — rerun `driver-status.ps1` to check.

## 5. Sources

- [libwdi/Zadig wiki](https://github.com/pbatard/libwdi/wiki/Zadig) — List
  All Devices, Ignore Hubs or Composite Parents, selecting the right
  interface, driver replacement warnings.
- [libusb Windows wiki](https://github.com/libusb/libusb/wiki/Windows) —
  WinUSB/libusbK/libusb0 backend support matrix.
- [libusb#759](https://github.com/libusb/libusb/issues/759) and
  [libusb#46](https://github.com/libusb/libusb/issues/46) — WinUSB
  isochronous maturity vs. libusbK on Windows.
- [libusb PR #284](https://github.com/libusb/libusb/pull/284) — WinUSB
  backend isochronous support landing in 1.0.23.
- [RTL-SDR: registry hack to stop Windows replacing a Zadig WinUSB
  driver](https://www.rtl-sdr.com/a-registry-hack-to-stop-windows-11-replacing-the-winusb-driver-installed-via-zadig/)
  — `ExcludeWUDriversInQualityUpdate`, and the general "Windows Update
  reinstalls the vendor driver" failure mode for Zadig-managed devices.
- This repo: `docs/usb-descriptors.md` (endpoint/transfer-type map),
  `docs/startup.md` (init sequence, confirms single-interface, no
  host-pushed firmware), `HANDOFF.md` (endpoint roles).
- This machine, read-only: `pnputil /enum-drivers`, `C:\Windows\INF\oem74.inf`
  /`oem84.inf`/`oem85.inf`/`oem86.inf`/`oem87.inf`, `sc qc`/`sc query`,
  `reg query` on the USB/Media class filter keys, installed-program registry
  keys (`Pinnacle Video Driver` MSI).
