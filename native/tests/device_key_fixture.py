"""Run only under a fresh dbus-run-session; never use the desktop keyring."""
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main():
    with tempfile.TemporaryDirectory(prefix="nordssh-keyring-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        for key, child in (("HOME", "home"), ("XDG_DATA_HOME", "data"),
                           ("XDG_CONFIG_HOME", "config"), ("XDG_RUNTIME_DIR", "runtime")):
            path = root / child
            path.mkdir(mode=0o700)
            env[key] = str(path)
        daemon = subprocess.Popen(
            ["gnome-keyring-daemon", "--foreground", "--unlock", "--components=secrets"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        try:
            daemon.stdin.write(b"synthetic-test-keyring-password\n")
            daemon.stdin.close()
            for _ in range(100):
                # NameHasOwner does not auto-activate a service using the bus
                # launcher's inherited (non-test) HOME/keyring environment.
                result = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                    "--object-path", "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.NameHasOwner", "org.freedesktop.secrets"],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=2)
                if result.returncode == 0 and b"true" in result.stdout:
                    break
                if daemon.poll() is not None:
                    raise RuntimeError("Isolated keyring stopped")
                time.sleep(0.05)
            else:
                raise RuntimeError("Isolated keyring unavailable")
            return subprocess.run([sys.argv[1]], env=env, timeout=65).returncode
        finally:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
