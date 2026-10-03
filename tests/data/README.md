# Test fixtures

Small recordings the replay tests run against. They are **not committed**
(`.gitignore`); without them the tests that need them skip themselves, and the
rest of the suite runs as normal.

| File | What it is | Used by |
|---|---|---|
| `ep88-pal.bin` | First 2 MiB of a raw EP 0x88 dump, PAL DV (colour bars) | reassembler baseline, `test_replay_*`, `test_pin_check_output`, `test_dv_rewrap` |
| `ep88-ntsc.bin` | First 2 MiB of a raw EP 0x88 dump, NTSC DV, 32 kHz 12-bit audio | reassembler baseline, `test_dv_rewrap` |
| `dv-ntsc.dv` | 20 frames of a reassembled NTSC DV file, 48 kHz 16-bit audio | `test_dv_audio_vs_libavformat` |
| `dv-ntsc-32k.dv` | 150 frames of a camera's NTSC DV, 32 kHz 12-bit audio, 4 channels | `test_dv_audio_vs_libavformat` |
| `hdv.ts` | First 16,000 TS packets of an HDV capture (1440x1080 25p MPEG-2 + MP2) | `test_replay_hdv`, `test_hdv_rewrap` |

Make your own with `MarvinCaptureCLI --capture file.dv` (or `.ts`). The raw
`ep88-*.bin` dumps came from the raw endpoint dump that is now disabled
(`#if 0` in `src/core/pinnacle_stream.c`, see docs/usage.md). Cut them short: a couple of
MiB is plenty. If you replace the `ep88-*.bin` files, regenerate the hashes in
`tests/baseline_sha256.txt` (see its header).
