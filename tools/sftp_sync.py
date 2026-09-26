# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version. This program is distributed WITHOUT ANY WARRANTY;
# see <https://www.gnu.org/licenses/> for the full licence.

import os, sys, paramiko, posixpath

# Capture-host connection, from the environment so no credentials live in git.
#   export PIN_HOST=192.168.0.103 PIN_USER=jonas PIN_PASS=...
# PIN_PASS may be omitted if you have SSH keys set up.
HOST = os.environ.get("PIN_HOST", "192.168.0.103")
USER = os.environ.get("PIN_USER", "jonas")
PASS = os.environ.get("PIN_PASS") or None

def connect():
    transport = paramiko.Transport((HOST, 22))
    transport.connect(username=USER, password=PASS)
    return transport, paramiko.SFTPClient.from_transport(transport)

def mkdirs(sftp, remote_dir):
    parts = remote_dir.split("/")
    cur = ""
    for part in parts:
        if not part:
            continue
        cur += "/" + part
        try:
            sftp.stat(cur)
        except FileNotFoundError:
            sftp.mkdir(cur)

def push(local_root, remote_root):
    transport, sftp = connect()
    try:
        for dirpath, dirnames, filenames in os.walk(local_root):
            # Windows relpaths use backslashes, which the remote would take as
            # part of a file name.
            rel = os.path.relpath(dirpath, local_root).replace(os.sep, "/")
            remote_dir = posixpath.normpath(posixpath.join(remote_root, rel)) if rel != "." else remote_root
            dirnames[:] = [d for d in dirnames if d != "__pycache__"]
            mkdirs(sftp, remote_dir)
            for fn in filenames:
                local_path = os.path.join(dirpath, fn)
                remote_path = posixpath.join(remote_dir, fn)
                print(f"pushing {local_path!r} -> {remote_path!r}")
                sftp.put(local_path, remote_path)
                print(f"pushed {local_path} -> {remote_path}")
    finally:
        sftp.close()
        transport.close()

def pull(remote_path, local_path):
    transport, sftp = connect()
    try:
        sftp.get(remote_path, local_path)
        print(f"pulled {remote_path} -> {local_path}")
    finally:
        sftp.close()
        transport.close()

if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "push":
        push(sys.argv[2], sys.argv[3])
    elif mode == "pull":
        pull(sys.argv[2], sys.argv[3])
