import os, sys, base64, paramiko

# Capture-host connection, from the environment so no credentials live in git.
#   export PIN_HOST=192.168.0.103 PIN_USER=jonas PIN_PASS=...
# PIN_PASS may be omitted if you have SSH keys set up.
HOST = os.environ.get("PIN_HOST", "192.168.0.103")
USER = os.environ.get("PIN_USER", "jonas")
PASS = os.environ.get("PIN_PASS") or None

def _connect():
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=PASS, timeout=15)
    return client

# NOTE: paramiko raises socket.timeout once a channel read blocks this long,
# which SIGHUPs whatever is running on the far side. Anything longer than this
# must be launched detached -- see tools/remote/longrun.sh.
def run(cmd, sudo=False, timeout=120):
    client = _connect()
    if sudo:
        cmd = "sudo -S -p '' " + cmd
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout)
    if sudo and PASS:
        stdin.write(PASS + "\n")
        stdin.flush()
    out = stdout.read().decode(errors="replace")
    err = stderr.read().decode(errors="replace")
    rc = stdout.channel.recv_exit_status()
    client.close()
    return rc, out, err

def run_script(script_text, sudo=False, timeout=120, interpreter="bash"):
    b64 = base64.b64encode(script_text.encode()).decode()
    remote_path = "/tmp/_sshrun_script_%d" % (hash(script_text) & 0xffffffff)
    decode_cmd = f"echo {b64} | base64 -d > {remote_path} && chmod +x {remote_path}"
    rc, out, err = run(decode_cmd, sudo=False, timeout=30)
    if rc != 0:
        return rc, out, err

    if interpreter == "bash":
        run_cmd = f"bash {remote_path}"
    elif interpreter == "python3":
        run_cmd = f"python3 {remote_path}"
    else:
        raise ValueError(interpreter)

    rc, out, err = run(run_cmd, sudo=sudo, timeout=timeout)
    run(f"rm -f {remote_path}", sudo=False, timeout=15)
    return rc, out, err

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "cmd"
    if mode == "cmd":
        cmd = sys.argv[2]
        sudo = "--sudo" in sys.argv
        rc, out, err = run(cmd, sudo=sudo)
        print(out)
        if err:
            print("STDERR:", err, file=sys.stderr)
        sys.exit(rc)
    elif mode == "script":
        path = sys.argv[2]
        sudo = "--sudo" in sys.argv
        interp = "python3" if path.endswith(".py") else "bash"
        with open(path) as f:
            text = f.read()
        rc, out, err = run_script(text, sudo=sudo, interpreter=interp, timeout=600)
        print(out)
        if err:
            print("STDERR:", err, file=sys.stderr)
        sys.exit(rc)
