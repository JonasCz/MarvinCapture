# Tools

## Verification

### `dvcheck.py` — structural + timecode check of a captured `.dv`

```bash
python3 tools/dvcheck.py out.dv --duration-seconds 300
python3 tools/dvcheck.py out.dv --dump-packs      # what metadata the camera emits
```

Verifies the file is a whole number of 12,000-byte DIF sequences, that each
sequence is where its Dseq says it should be, that all 150 blocks per sequence
carry the section type and DBN the DV spec requires, that no sequence is the
all-zero placeholder the reassembler writes for a gap, and that the frame count
matches wall-clock duration. Exits non-zero if anything is wrong.

This is independent of the CIP/DBC continuity counter `pincli` reports, so the
two cross-check each other. See
[../docs/capture-reliability.md](../docs/capture-reliability.md).

## Driving the capture host

The development rig is a separate Ubuntu machine with the device attached.
These talk to it over SSH. **Credentials come from the environment**, never
from the repo:

```bash
export PIN_HOST=192.168.0.103
export PIN_USER=jonas
export PIN_PASS=...        # omit if you use SSH keys
```

| Script | Use |
|---|---|
| `sshrun.py cmd "<command>" [--sudo]` | run one command |
| `sshrun.py script <file.sh\|file.py> [--sudo]` | upload and run a script |
| `sftp_sync.py push <local_dir> <remote_dir>` | push a **directory** (a single file path silently does nothing) |
| `sftp_sync.py pull <remote_file> <local_file>` | pull one file |

Two traps worth knowing:

- `sshrun.py` reads with a **120 s timeout**, and when it fires the channel
  closes and SIGHUPs whatever is running on the far side. Anything longer must
  be launched detached — that is what `remote/longrun.sh` is for.
- `sudo` cannot run shell builtins, so `sshrun.py cmd "cd /x && ..." --sudo`
  fails. Use absolute paths, or `env VAR=... /full/path/to/binary`.

On Git Bash/MSYS, prefix with `MSYS_NO_PATHCONV=1` or absolute Unix paths get
mangled into Windows ones.

## Scripts that run *on* the capture host (`remote/`)

| Script | Use |
|---|---|
| `longrun.sh [secs] [out] [log]` | Launch a capture fully detached (`setsid nohup`) so an SSH timeout can't kill it. Prints the pid and returns. Use for anything over ~100 s. |
| `sigint.sh` | Exercise the real Ctrl+C path: start a capture with no `-t`, send SIGINT, time the shutdown, and confirm the file ends on a whole frame boundary. |
| `ffcheck.sh [file]` | Independent verification with ffmpeg: list streams, full decode reporting only errors, and count decoded frames. A healthy capture yields 0 errors and one `Detected timecode is invalid` line (expected — the camera emits no timecode in live view). |

Run them with `sshrun.py script tools/remote/<name>.sh --sudo`.

## Windows USB capture (historical)

`capture-usb.ps1` and `etw-usb.ps1` are from the abandoned bare-metal Windows
capture attempt. USBPcap recorded **nothing** from this device, not even
enumeration. Kept only so nobody retries that path; see
[../docs/capture-tooling.md](../docs/capture-tooling.md).
