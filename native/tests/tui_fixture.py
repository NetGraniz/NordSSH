"""Disposable Linux PTYs and real TUI programs. No user hosts/configs/clipboard.

This is a test driver, not a subprocess backend for NordSSH. Programs run on
isolated PTYs; their output and terminal replies pass through the Qt/VT adapter.
It proves local program compatibility, not SSH cancellation or physical input.
"""
import argparse
import errno
import fcntl
import os
import pathlib
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time

KEY_ENTER, KEY_ESCAPE = 0x01000004, 0x01000000
CTRL = 0x04000000


class Fixture:
    def __init__(self, driver, command, root, qpa):
        self.tmux_socket = root / f"tmux-{os.getuid()}" / "nordssh-fixture" if command[0] == "tmux" else None
        self.driver = subprocess.Popen([driver], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       env={**os.environ, "QT_QPA_PLATFORM": qpa})
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        env = {**os.environ, "HOME": str(root), "XDG_CONFIG_HOME": str(root),
               "TERM": "xterm-256color", "LC_ALL": "C.UTF-8", "SHELL": "/bin/sh",
               "ENV": "", "BASH_ENV": "", "TMUX_TMPDIR": str(root), "PS1": "NORD-PTY> "}

        def child_tty():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        self.app = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                                    cwd=root, env=env, preexec_fn=child_tty)
        os.close(slave)
        try:
            if self.request(b"P") != qpa.encode():
                raise AssertionError("Requested Qt platform was not used")
        except Exception:
            self.close()
            raise

    def exact(self, size):
        result = b""
        deadline = time.monotonic() + 5
        while len(result) < size:
            if time.monotonic() > deadline:
                raise TimeoutError("Native TUI driver stalled")
            if not select.select([self.driver.stdout], [], [], 0.1)[0]:
                continue
            chunk = os.read(self.driver.stdout.fileno(), size - len(result))
            if not chunk:
                raise AssertionError("Native TUI driver closed unexpectedly")
            result += chunk
        return result

    def request(self, kind, data=b""):
        self.driver.stdin.write(kind + struct.pack("!I", len(data)) + data)
        self.driver.stdin.flush()
        header = self.exact(5)
        length = struct.unpack("!I", header[1:])[0]
        if length > 262144:
            raise AssertionError("Oversized native driver reply")
        return self.exact(length)

    def pump(self, wait=0.05):
        if not select.select([self.master], [], [], wait)[0]:
            return
        try:
            chunk = os.read(self.master, 65536)
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            return
        if chunk:
            reply = self.request(b"F", chunk)
            if reply:
                os.write(self.master, reply)

    def expect(self, text):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.pump()
            if self.request(b"C", text.encode()) == b"1":
                return
        raise AssertionError(f"Visible terminal marker missing: {text!r}")

    def text(self, text):
        data = self.request(b"T", text.encode())
        if not data:
            raise AssertionError("Native text/paste produced no input")
        os.write(self.master, data)

    def literal(self, text):
        data = self.request(b"L", text.encode("ascii"))
        if not data:
            raise AssertionError("Native literal keys produced no input")
        os.write(self.master, data)

    def key(self, key, mods=0):
        data = self.request(b"K", struct.pack("!II", key, mods))
        if not data:
            raise AssertionError("Native key produced no input")
        os.write(self.master, data)

    def resize(self, columns=100, rows=30):
        self.request(b"Z", struct.pack("!II", columns, rows))
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        os.killpg(self.app.pid, signal.SIGWINCH)

    def exited(self):
        deadline = time.monotonic() + 5
        while self.app.poll() is None and time.monotonic() < deadline:
            self.pump()
        if self.app.poll() is None:
            raise AssertionError("TUI application did not exit")
        if self.app.returncode != 0:
            raise AssertionError(f"TUI application failed: {self.app.returncode}")
        for _ in range(4):
            self.pump(0)

    def close(self):
        # Only our recorded fixture process group; never a name-based kill.
        if self.app.poll() is None:
            os.killpg(self.app.pid, signal.SIGTERM)
            try:
                self.app.wait(2)
            except subprocess.TimeoutExpired:
                os.killpg(self.app.pid, signal.SIGKILL)
                self.app.wait(2)
        os.close(self.master)
        if self.tmux_socket and self.tmux_socket.exists():
            subprocess.run(["tmux", "-S", str(self.tmux_socket), "kill-server"], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
        self.driver.stdin.close()
        try:
            self.driver.wait(3)
        except subprocess.TimeoutExpired:
            self.driver.kill()
            self.driver.wait(2)
        if self.driver.returncode:
            raise AssertionError("Native terminal driver failed on shutdown")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("driver")
    parser.add_argument("--qpa", default="offscreen", choices=["offscreen", "xcb", "wayland"])
    args = parser.parse_args()
    driver = str(pathlib.Path(args.driver).resolve())
    with tempfile.TemporaryDirectory(prefix="nordssh-tui-") as temp:
        root = pathlib.Path(temp)
        source = root / "fixture.txt"
        scenarios = ["vim", "nano", "less", "top", "tmux"]
        for name in scenarios:
            source.write_text("SOURCE-MARKER\n", encoding="utf-8")
            if name == "vim":
                command = ["vim", "-Nu", "NONE", "-n", "-i", "NONE", "-N", str(source)]
            elif name == "nano":
                command = ["nano", "--ignorercfiles", str(source)]
            elif name == "less":
                source.write_text("".join(f"LESS-LINE-{i:03d}\n" for i in range(150)), encoding="utf-8")
                command = ["less", "-R", str(source)]
            elif name == "top":
                command = ["top", "-d", "1"]
            else:
                command = ["tmux", "-L", "nordssh-fixture", "-f", "/dev/null", "new-session", "/bin/sh"]
            fixture = Fixture(driver, command, root, args.qpa)
            try:
                if name in ("vim", "nano"):
                    fixture.expect("SOURCE-MARKER")
                    if name == "vim":
                        fixture.literal("i")
                    fixture.text("EDIT-MARKER ")
                    fixture.resize()
                    fixture.expect("EDIT-MARKER")
                    if name == "vim":
                        fixture.key(KEY_ESCAPE)
                        fixture.literal(":wq")
                        fixture.key(KEY_ENTER)
                    else:
                        fixture.key(ord("O"), CTRL)
                        fixture.expect("File Name to Write")
                        fixture.key(KEY_ENTER)
                        fixture.key(ord("X"), CTRL)
                    fixture.exited()
                    if "EDIT-MARKER" not in source.read_text(encoding="utf-8"):
                        raise AssertionError("Editor did not save native terminal input")
                elif name == "less":
                    fixture.expect("LESS-LINE-000")
                    fixture.literal("G"); fixture.expect("LESS-LINE-149")
                    fixture.resize(); fixture.literal("g"); fixture.expect("LESS-LINE-000")
                    fixture.literal("q"); fixture.exited()
                elif name == "top":
                    fixture.expect("Tasks:"); fixture.resize(); fixture.expect("PID")
                    fixture.literal("q"); fixture.exited()
                else:
                    fixture.expect("NORD-PTY>")
                    fixture.text("printf 'TMUX-FIRST\\n'"); fixture.key(KEY_ENTER); fixture.expect("TMUX-FIRST")
                    fixture.key(ord("B"), CTRL); fixture.literal("c"); fixture.expect("NORD-PTY>")
                    fixture.text("printf 'TMUX-SECOND\\n'"); fixture.key(KEY_ENTER); fixture.expect("TMUX-SECOND")
                    fixture.resize(); fixture.key(ord("B"), CTRL); fixture.literal("p"); fixture.expect("TMUX-FIRST")
                    fixture.text("exit"); fixture.key(KEY_ENTER)
                    fixture.text("exit"); fixture.key(KEY_ENTER); fixture.exited()
                print(f"PASS {args.qpa}: real {name}, native VT/Qt raster/input and resize", flush=True)
            finally:
                fixture.close()


if __name__ == "__main__":
    main()
