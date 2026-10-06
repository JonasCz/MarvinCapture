<!-- Generated from docs/cli.md.template by scripts/sync-cli-help.sh (run by the build): edit the template, not this file. -->
## Help text

The text printed by `--help` (followed by the device list). It is defined once,
in `src/engine/pin_script.c`; the build (`scripts/build.sh`, `scripts/build.ps1`)
fills it in here, so edit the C source, not this block. On a
terminal the CLI wraps it to the window width; piped, it is left unwrapped.

```text

```

## Running the GUI from a shell

`MarvinCaptureGUI.exe` is a GUI-subsystem program, so PowerShell returns to the
prompt immediately and its console output (for example `--debug`) interleaves
with the prompt. Pipe the output to make the shell wait until the app exits and
to keep the output in order:

```powershell
.\MarvinCaptureGUI.exe --debug | Out-Host
.\MarvinCaptureGUI.exe --debug 2>&1 | Tee-Object gui.log   # also save a log
```

With stdout redirected or piped the app writes to that; otherwise it attaches
to the parent console. Started from Explorer there is no output.
Add `--exit-when-done` to close the window when the command-line steps finish;
the process exit code is the one listed in the help text (the CLI always exits
when done).
