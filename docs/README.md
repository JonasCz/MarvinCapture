# Documentation

**Using it**

- [usage.md](usage.md): running captures, telling device faults from a quiet
  camera, proving nothing was dropped, limits of long captures, diagnostics.
- [windows-driver.md](windows-driver.md): binding the device to WinUSB (Zadig).
- [../gui/windows/README.md](../gui/windows/README.md): the GUI and its command line.
- [building.md](building.md): building, output layout, tests, repo layout.
- [ci.md](ci.md): the GitHub Actions build (installers for all platforms, releases).
- [windows-size.md](windows-size.md): why the Windows installer is big, the trimming/pruning setup and how to test it.

**How the device works**

- [hardware.md](hardware.md): USB descriptors, endpoints, initialisation, what
  the FPGA is.
- [protocol.md](protocol.md): the command word, OHCI register access, the two
  layers of EP 0x88 framing, behaviours that matter.
- [startup.md](startup.md): every step from plug-in to the first stream byte.
- [deck-control.md](deck-control.md): play/stop/FF/REW over AV/C.
- [hdv.md](hdv.md): HDV over FireWire, MPEG-2 transport stream output.
- [analog.md](analog.md): analog capture, the SAA7113 / AC'97 / capture-block
  registers, clocks and A/V sync; also the vendor driver, the other Marvin
  models and the three bitstreams.

**Testing**

- [testing-required.md](testing-required.md): what still needs testing, grouped by the
  input that tests it: HDV 1080i, 720p and 24p sources, DV variants, mixed tapes, other
  cameras and decks, a FireWire PC or second Marvin as the source, analog, drives, hosts,
  and what is left to test in code.

**Handoff notes**

- [handoff/linux-testing.md](handoff/linux-testing.md): what to test on the Linux host, and how.
- [handoff/macos-cpu-usage.md](handoff/macos-cpu-usage.md): macOS GUI CPU results and what remains.
- [handoff/dv-timecode-warning.md](handoff/dv-timecode-warning.md): the DV "invalid timecode" warning.
- [handoff/android-plan.md](handoff/android-plan.md): feasibility and plan for an Android GUI (not started).
- [playout.md](playout.md): MarvinPlayout, DV/HDV playout and a virtual deck over files (usage, timing model, test plan).
- [handoff/playout-feasibility.md](handoff/playout-feasibility.md): the vendor-driver analysis behind it (isochronous
  transmit on EP 0x06, AV/C target), analog playout; the device side has not run on hardware.

**Elsewhere**

- [../firmware/README.md](../firmware/README.md): the FPGA bitstreams and their licence.
- [../third_party/README.md](../third_party/README.md): the vendored FFmpeg.
- [../src/engine/README.md](../src/engine/README.md): the hardware-free engine modules.
