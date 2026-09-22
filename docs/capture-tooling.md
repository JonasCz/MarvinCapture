# Capturing USB traffic from the Pinnacle 500-USB — what works, what doesn't

Investigated 2026-08-22 on the live device (Windows 10 Pro 19045, AMD system, all-xHCI).

## The headline problem

**USBPcap 1.5.4.0 records zero packets from the Pinnacle while the device is
demonstrably streaming 10.47 MB/s.** This was proven, not guessed, by running
USBPcap and Windows' own ETW xHCI tracing over the *same* 15-second window.

| Capture method | Result over identical 15 s |
|---|---|
| ETW `Microsoft-Windows-USB-USBXHCI` | slot 3, endpoint **0x88 IN**, 8,043 transfers, **164,720,640 bytes = 10.47 MB/s** |
| USBPcap on the same root hub | **0 packets** from the Pinnacle (other devices on that hub captured fine) |

USBPcap missed even the **low-rate control transfers** to the device. That rules out a
buffer-overflow / bandwidth explanation: if it were dropping under load, the small EP0
traffic would still appear. The filter simply is not attached to this device's stack.

USBPcap can still *enumerate* the device and inject its descriptors, because that is
done by querying the hub — it does not require a filter on the device. So seeing the
Pinnacle in USBPcap's device tree tells you nothing about whether it will be captured.

## How the attribution was proven

xHCI slot IDs are **per-controller**, and this machine has two:

| Root hub instance | USBPcap iface | PCI location | ETW controller handle |
|---|---|---|---|
| `5&201a262c&0&0` | `\\.\USBPcap1` | PCI bus 7, dev 0, fn 0 | `0x3771d94ee578` |
| `5&2eda9e40&0&0` | `\\.\USBPcap2` | PCI bus 45, dev 0, fn 3 | `0x3771d93b2bc8` |
| `1&2b53a856&0&0` | `\\.\USBPcap3` | (virtual, `ROOT\USB\0000`) | — |

The Pinnacle sits on `5&2eda9e40&0&0` → **PCI bus 45** → the *second* controller.
Controller 1 also has a slot 3 (port 9, a HID keyboard), which is why the raw ETW
aggregate looks ambiguous at first glance. A keyboard does not move 20,480-byte bulk
transfers, so the 10.47 MB/s stream is unambiguously the Pinnacle.

## What ETW gives us (and what it doesn't)

`Microsoft-Windows-USB-USBXHCI` {30E1D284-5D88-459C-83FD-6345B39B19EC} works
perfectly and needs no third-party driver:

- Event **Id 41** = transfer submitted: `fid_SlotId`, `fid_EndpointContextIndex`,
  `fid_StreamId`, `fid_BytesTotal`
- Event **Id 45** = transfer completed: slot / endpoint context / stream
- Event **Id 4** = device info: `fid_SlotId`, `PortPath`, `DeviceSpeed`,
  `fid_ConfigurationValue`, **`fid_AlternateSetting`**
- Event **Id 3** = controller info incl. `fid_PciBus`

Decode `fid_EndpointContextIndex` (the xHCI Device Context Index) as:
`DCI 1` = EP0 control; otherwise `EP = floor(DCI/2)`, direction `IN` if DCI is odd.
So **DCI 17 → EP 8 IN → 0x88**, which matches alt setting 1 in the descriptor.

**Limitation: ETW gives transfer metadata only — no payload bytes.** It is excellent
for diagnosis, throughput measurement, and confirming which endpoint and alt setting a
capture uses, but it cannot by itself reverse-engineer the wire protocol.

Run it with:

```
logman create trace usbtrace -ets -p "{30E1D284-5D88-459C-83FD-6345B39B19EC}" 0xffffffffffffffff 0xff -o usb.etl -nb 128 640 -bs 1024 -max 512
logman stop usbtrace -ets
```

Read it in PowerShell with `Get-WinEvent -Path usb.etl -Oldest` (no manifest juggling
needed). **`tools/etw-usb.ps1` does all of this for you** — self-elevates, traces,
and prints controllers, the slot→port map, and per-endpoint throughput.

## Confirmed facts about the device's streaming behaviour

- Streams on **bulk EP 0x88 IN**, in **20,480-byte** transfers, ~536 transfers/s.
- Raw buffer rate 10.47 MB/s. Note this is *requested* buffer size, not necessarily
  bytes filled — DV is ~3.6 MB/s, so the device likely partially fills each 20 KB
  buffer, or the stream carries more than just DV essence. Worth measuring precisely
  once payload capture works.
- EP 0x88 exists only in **alternate setting 1** (see `usb-descriptors.md`), so the
  driver selects **alt 1** for capture — not alt 3 as previously guessed.
- The device is completely silent when idle (no keepalive polling at all).

## Things that were tried and did NOT work

- Running Wireshark elevated. (Required, but not sufficient.)
- Capturing on all three root hubs simultaneously.
- `--inject-descriptors`, full snaplen, 128 MB buffer.
- **Forcing re-enumeration through an already-running capture**
  (`pnputil /restart-device "USB\VID_2304&PID_0213\6&1B0A3985&0&1"`). USBPcap recorded
  **zero** packets of the enumeration that Windows demonstrably performed — no
  `GET_DESCRIPTOR`, no `SET_ADDRESS`, no `SET_CONFIGURATION`. This is the strongest
  evidence that USBPcap's filter never attaches to this device at all.
  Note: re-enumeration tears down the driver's streaming session, so WinDV stops
  previewing and must be restarted afterwards.

## Options for getting payload capture

0. **Move the device and retry — free, and worth doing first.** Two observations
   narrow the fault: on the PCI-bus-45 controller the directly-attached Pinnacle is
   invisible while a device *behind an external hub on that same root hub* captures
   fine (600 packets); and on the PCI-bus-7 controller a directly-attached device
   (the ROG mouse) captures fine. So try, in order:
   a. plug the Pinnacle into the **external "Generic USB Hub"**, which gives USBPcap a
      standard USB 2.0 hub stack to filter instead of the xHCI root hub's internal
      path — this is a well-known workaround;
   b. failing that, move it to a port on the **other controller** (`\\.\USBPcap1`).
   Neither has been tested with the device actually streaming yet.
2. **A device-stack analyzer instead of a hub-stack one.** USBPcap filters the *hub*;
   tools like USBlyzer or HHD Device Monitoring Studio install an upper filter on the
   *device's own* stack and therefore see URBs USBPcap structurally cannot. Both are
   commercial with trials — for a one-off RE job that is a reasonable trade.
3. **Linux host + Windows VM with USB passthrough.** Install the Pinnacle driver inside
   a Windows guest under QEMU/KVM, pass the device through, and capture on the Linux
   host with `usbmon` (native Wireshark support, full payload, handles high bandwidth).
   This is the gold standard for this kind of work and sidesteps Windows filter
   problems entirely.
4. **An older machine with a real EHCI (USB 2.0) controller.** USBPcap dates from the
   USB 2.0 era and is markedly more reliable against `USBPORT` than against xHCI.
   This is a USB 2.0 device, so nothing is lost by using a USB 2.0 host.
