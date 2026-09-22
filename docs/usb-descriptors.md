# Pinnacle 500-USB — USB descriptor map (confirmed)

Captured 2026-08-22 via USBPcap `--inject-descriptors` on a live device
(`USB\VID_2304&PID_0213`). Raw bytes: `scratchpad/pinnacle_capture.pcap`, frame 46.

## Configuration descriptor

`09 02 a4 00 01 01 00 80 96`

| Field | Value |
|---|---|
| wTotalLength | 164 |
| bNumInterfaces | **1** |
| bConfigurationValue | 1 |
| bmAttributes | 0x80 (bus powered) |
| bMaxPower | 0x96 → **300 mA** |

One interface, **class 0xFF (vendor-specific)**, with **four alternate settings**.

## Endpoint map — all four alt settings

Every endpoint is **bulk** (`bmAttributes = 0x02`), **wMaxPacketSize = 512**,
`bInterval = 0`. **There is not a single isochronous endpoint on this device.**

### Alt 0 — 3 endpoints (idle / control)
| EP | Dir | Type | MaxPacket |
|---|---|---|---|
| 0x01 | OUT | bulk | 512 |
| 0x81 | IN | bulk | 512 |
| 0x02 | OUT | bulk | 512 |

### Alt 1 — 6 endpoints (full duplex A/V — capture + playback?)
| EP | Dir | Type | MaxPacket |
|---|---|---|---|
| 0x01 | OUT | bulk | 512 |
| 0x81 | IN | bulk | 512 |
| 0x02 | OUT | bulk | 512 |
| 0x84 | IN | bulk | 512 |
| 0x06 | OUT | bulk | 512 |
| 0x88 | IN | bulk | 512 |

### Alt 2 — 4 endpoints (3 OUT, 1 IN — playback/DV-out to camera?)
| EP | Dir | Type | MaxPacket |
|---|---|---|---|
| 0x01 | OUT | bulk | 512 |
| 0x81 | IN | bulk | 512 |
| 0x02 | OUT | bulk | 512 |
| 0x06 | OUT | bulk | 512 |

### Alt 3 — 4 endpoints (3 IN, 1 OUT — **most likely the capture mode**)
| EP | Dir | Type | MaxPacket |
|---|---|---|---|
| 0x01 | OUT | bulk | 512 |
| 0x81 | IN | bulk | 512 |
| 0x82 | IN | bulk | 512 |
| 0x86 | IN | bulk | 512 |

## Why this matters for the project

1. **All-bulk, no isochronous.** This is the single best piece of news so far.
   Bulk transfers are retried by the host controller, so the "lossless DV transfer"
   claim is structurally guaranteed by the transport. It also means WinUSB/libusb
   support is trivial — no isochronous packet scheduling, no WinUSB iso caveats.
2. **Vendor-specific class (0xFF), single interface.** Nothing in Windows will fight
   us for the interface; a WinUSB INF binds cleanly.
3. **Endpoint numbers 1, 2, 4, 6, 8** map exactly onto the CY7C68013A (FX2LP)
   endpoint set — EP1 plus the four large slave-FIFO endpoints EP2/4/6/8. Consistent
   with the FX2 doing plain FIFO streaming and the FPGA doing the framing.
4. **Alt-setting switching is the mode selector.** Expect the vendor driver to issue
   `SET_INTERFACE` to alt 3 (or 1) when a DV capture starts. That single control
   transfer is probably the "start streaming" trigger we need to replicate — worth
   watching for specifically in the Phase 0 trace.
5. Educated guess to verify: **EP 0x01/0x81 is the command/response channel**
   (present in every alt setting, so it is the always-on control path — likely where
   the tunneled 1394 async / AV-C traffic rides), while the large FIFOs
   (0x82/0x84/0x86/0x88) carry bulk DV payload.
6. The device enumerates with its full vendor descriptor set as `2304:0213`, **not**
   as a bare Cypress `04B4:8613`. That suggests the FX2 8051 firmware is loaded from
   an on-board I2C EEPROM rather than pushed by the host at every plug-in — which
   would remove most of Phase 1 from the plan. **Must be confirmed with a plug-in
   trace on a machine where the Pinnacle driver is not installed.**

## Open questions for the next trace

- Does the host push FX2 firmware and/or an FPGA bitstream at plug-in, or is the
  device self-booting? (Watch for large `0xA0` vendor writes at enumeration.)
- Which alt setting does a DV capture actually select?
- Which endpoint carries DV, and is the CIP/IEC 61883 header retained?
