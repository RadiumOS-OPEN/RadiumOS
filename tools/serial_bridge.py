#!/usr/bin/env python3
"""Shares the host clipboard with RadiumOS over the guest's serial port (QEMU -serial pty), using the OSC 52
sequence that terminals use for this:

  guest -> host   ESC ] 52 ; c ; <base64 text> BEL      sets the host clipboard
  guest asks      ESC ] 52 ; c ; ? BEL                  the bridge answers with the host clipboard in the same form

usage: serial_bridge.py /dev/pts/N
The clipboard tools are found automatically (wl-copy/wl-paste, xclip, xsel); RADIUM_CLIP_COPY and
RADIUM_CLIP_PASTE (shell commands) override them.
"""
import base64
import os
import select
import shutil
import subprocess
import sys
import termios
import tty

START = b"\x1b]52;c;"
MAX_TEXT = 3000          # what the guest can take in one paste
MAX_BUFFER = 1 << 20


def commands():
    copy = os.environ.get("RADIUM_CLIP_COPY")
    paste = os.environ.get("RADIUM_CLIP_PASTE")
    if copy and paste:
        return copy, paste
    if shutil.which("wl-copy") and shutil.which("wl-paste"):
        return "wl-copy", "wl-paste -n"
    if shutil.which("xclip"):
        return "xclip -selection clipboard -i", "xclip -selection clipboard -o"
    if shutil.which("xsel"):
        return "xsel -i -b", "xsel -o -b"
    return None, None


COPY, PASTE = commands()


def host_copy(data: bytes) -> None:
    if not COPY:
        return
    try:
        subprocess.run(COPY, shell=True, input=data, timeout=5,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError):
        pass


def host_paste() -> bytes:
    if not PASTE:
        return b""
    try:
        out = subprocess.run(PASTE, shell=True, capture_output=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return b""
    return out[:MAX_TEXT]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd, termios.TCSANOW)

    buffer = b""
    while True:
        try:
            ready, _, _ = select.select([fd], [], [])
            chunk = os.read(fd, 4096) if ready else b""
        except OSError:
            return 0          # QEMU closed the port
        if not chunk:
            return 0
        buffer += chunk

        while True:
            at = buffer.find(START)
            if at < 0:
                buffer = buffer[-(len(START) - 1):] if len(buffer) >= len(START) else buffer
                break
            end = buffer.find(b"\x07", at + len(START))
            if end < 0:
                buffer = buffer[at:]
                if len(buffer) > MAX_BUFFER:
                    buffer = b""
                break
            payload = buffer[at + len(START):end]
            buffer = buffer[end + 1:]
            if payload == b"?":
                reply = START + base64.b64encode(host_paste()) + b"\x07"
                os.write(fd, reply)
            else:
                try:
                    host_copy(base64.b64decode(payload, validate=True))
                except ValueError:
                    pass


if __name__ == "__main__":
    sys.exit(main())
