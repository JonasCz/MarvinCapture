# Driving the Windows VM, and moving data on and off it

Everything is done from the Ubuntu host `$PIN_HOST` (see tools/README.md).
The guest is the VirtualBox VM **`Win7`**, which auto-logs in as user **`user`**.
Actions are visible in the user's RDP session, so they can watch along.

## Why not `guestcontrol`

Guest Additions 7.0.20 *are* installed, but `VBoxManage guestcontrol` is unusable: the
auto-login account `user` has a **blank password**, and Windows policy refuses
blank-password secondary logons. Every attempt returns
*"The specified user was not able to logon on guest"*. The built-in `Administrator`
returns *"account … is restricted"*.

So control is done by **keyboard injection**, with a **shared folder as the return
channel**. No credentials needed.

## `vmctl` — the helper

Installed at `/usr/local/bin/vmctl` on the Ubuntu host. Run it with `sudo`.

```bash
sudo vmctl exec 'dir C:\Users\user\Desktop'
```

| Command | Does |
|---|---|
| `vmctl shot [out.png]` | screenshot the guest via `VBoxManage controlvm … screenshotpng` |
| `vmctl key <hex…>` | raw scancodes |
| `vmctl type <string>` | `keyboardputstring` |
| `vmctl close` | Alt+F4 the focused window |
| `vmctl run <cmdline>` | Win+R → type → Enter (fire and forget) |
| `vmctl exec <cmd>` | run a command, wait, **print its stdout/stderr back on the Linux side** |
| `vmctl start-windv` | launch WinDV (path read from `/etc/vmctl.windv`) |
| `vmctl stop-windv` | `taskkill /IM WinDV.exe /F` |
| `vmctl is-windv` | is WinDV running? |

WinDV lives at **`C:\Users\user\Desktop\WinDV.exe`** (recorded in `/etc/vmctl.windv`).

### How `exec` works

Win+R → **Ctrl+A** (so leftover text is replaced, not appended) → type
`cmd /c "(<your cmd>) > Z:\vmctl_out.txt 2>&1"` → Enter → poll
`/home/jonas/Desktop/vm-shared/vmctl_out.txt` on the Linux side → print it.

Verify with `vmctl shot` if something looks wrong; a screenshot of the Run dialog
before pressing Enter is the fastest way to catch a typing/quoting problem.

## The shared folder — three traps

Host path **`/home/jonas/Desktop/vm-shared`**, share name **`vm-shared`**, appears in
the guest as the automounted drive **`Z:`**.

1. **It is a *transient* mapping — it disappears when the VM powers off.** Re-add it
   after every VM restart:

   ```bash
   VBoxManage sharedfolder add Win7 --name vm-shared --hostpath /home/jonas/Desktop/vm-shared --transient --automount
   ```

   A permanent mapping can only be added while the VM is **powered off** (otherwise:
   *"machine is already locked for a session"*). Doing that once would remove this
   chore.

2. **Use the drive letter `Z:`, never the `\\VBOXSVR\vm-shared` UNC.** The Run dialog
   launches commands **elevated**, and in that context the UNC does not resolve
   (*"The system cannot find the file specified"*) while `dir Z:\` works fine.
   Non-elevated Explorer *can* browse the UNC, which makes this confusing.

3. **Never run `net use Z: /delete`.** The mapping is created by Guest Additions'
   automount; deleting it cannot be undone with `net use Z: \\VBOXSVR\vm-shared`
   (fails with *System error 67*). Recovery is to remove and re-add the shared folder
   on the host, which re-triggers automount.

## Getting data off the guest

```
guest writes to Z:\        →  /home/jonas/Desktop/vm-shared  (Ubuntu)  →  SFTP to your machine
```

The guest's `Z:` *is* the Ubuntu directory — no copy step between them. From there,
`scratchpad/remote/fetch3.py` pulls files into `traces/`. (The user's `H:` drive is a
share on **their own Windows desktop**, not visible to the guest; it isn't needed.)

Onto the guest: drop the file into `/home/jonas/Desktop/vm-shared` and read it from
`Z:` inside Windows.

## Controlling the VM itself

```bash
VBoxManage controlvm Win7 acpipowerbutton     # clean shutdown (poll VMState until poweroff)
VBoxManage startvm Win7 --type gui            # needs the GUI session env, see below
VBoxManage controlvm Win7 screenshotpng /tmp/vm.png
```

`startvm --type gui` must run as `jonas` **with the desktop session's environment**,
or it cannot open a window:

```
DISPLAY=:0  XAUTHORITY=/run/user/1000/.mutter-Xwaylandauth.XXXXXX
```

The Xauthority filename is random per session — read it at runtime from any process
owned by `jonas`:

```bash
for p in $(pgrep -u jonas); do tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -m1 '^XAUTHORITY=' && break; done
```

Detach/reattach the device without touching the VM window (as `jonas`, not root — it
talks to that user's VBoxSVC):

```bash
UUID=$(VBoxManage list usbhost | awk -v RS='' '/0x2304/{for(i=1;i<=NF;i++) if($i=="UUID:"){print $(i+1); exit}}')
VBoxManage controlvm Win7 usbdetach "$UUID"
VBoxManage controlvm Win7 usbattach "$UUID"
```

The UUID changes on physical replug, so always re-query it.

## SSH from the dev machine

`scratchpad/remote/sshrun.py` runs a script on the host over paramiko. It sends the
script **base64-encoded**, because naive quoting of multi-line scripts through
`bash -lc` mangles newlines. Use `--sudo` for root, `-f <file>` for a script file.

## Gotchas worth remembering

- The Run dialog **remembers history**; always send Ctrl+A before typing or your
  command gets appended to the previous one.
- Run-dialog commands execute **elevated** ("This task will be created with
  administrative privileges") — that is why `Z:` works but the UNC does not.
- Start-menu search is unreliable for launching WinDV: typing `windv` matched the
  `WinDV-1.2.3` **zip folder** instead of the executable. Use the full path via
  `vmctl start-windv`.
- Screenshots are the fastest debugging tool here. Two screenshots with different MD5s
  also prove that a preview is genuinely live rather than a frozen frame.
