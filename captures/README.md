# Captures made by *this* driver

Raw DV elementary streams produced by `pincli`, as opposed to
[../traces/](../traces/) which holds usbmon recordings of the **vendor**
driver. These are the driver's own output, kept as regression references.

They are the user's own recordings and are excluded by `.gitignore`.

| File | What it is | State |
|---|---|---|
| `20260922-5min-verified.dv` | The 5-minute stability run. 8,967 frames NTSC, 1,076,040,000 bytes, MD5 `1daa0254d9ca71377730be0c9da33662`. | **The headline result.** 0 zero-filled sequences, 0 CIP/DBC discontinuities, `dvcheck.py` says `RESULT: clean`, and `ffmpeg -f null -` decodes it with 0 errors and counts exactly 8,967 frames. |
| `20260922-verified-16s.dv` | 500 frames NTSC, camera in live view. Cut from a 45 s run made with the queued receive path, the EP 0x88 stop drain, and partial-frame truncation all in place. | **Reference.** `dvcheck.py` says `RESULT: clean`: 0 zero-filled sequences, 0 bad headers, 0 bad section types, 0 bad DBNs. The run it came from reported 0 CIP/DBC discontinuities. |
| `20260922-clean-3s.dv` | 90 frames. First capture that decoded with zero ffmpeg errors, made once the two layers of EP 0x88 framing were understood. | Historical — correct, but predates the reliability work. |
| `20260922-test_clean.dv` | 70 frames, visibly corrupt. | Kept deliberately as the **before** picture: this is what the output looked like when EP 0x88 was treated as raw DV. |

## Checking one

```bash
python3 tools/dvcheck.py captures/20260922-verified-16s.dv --duration-seconds 16.7
ffmpeg -v error -i captures/20260922-verified-16s.dv -f null -
```

`ffmpeg` will print `Detected timecode is invalid` once. That is expected and
is not an error in the capture — the camera emits no timecode in live view.
See [../docs/capture-reliability.md](../docs/capture-reliability.md).
