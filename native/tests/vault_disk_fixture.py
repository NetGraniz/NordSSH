"""Bounded full-disk test on a 128 KiB disposable CI tmpfs, not the host disk."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    if os.environ.get("NORDSSH_ISOLATED_OPENSSH")!="1" or os.environ.get("GITHUB_ACTIONS")!="true":
        raise RuntimeError("tmpfs fixture requires an explicitly enabled disposable CI runner")
    with tempfile.TemporaryDirectory(prefix="nordssh-disk-") as directory:
        root=Path(directory).resolve()
        if root.parent!=Path(tempfile.gettempdir()).resolve() or not root.name.startswith("nordssh-disk-") or root.is_symlink():
            raise RuntimeError("Unexpected disk fixture root")
        subprocess.run(["sudo","-n","mount","-t","tmpfs","-o",f"size=128k,mode=0700,uid={os.getuid()},gid={os.getgid()}","nordssh-synthetic",str(root)],check=True,timeout=10)
        try:
            return subprocess.run([sys.argv[1],str(root)],timeout=45).returncode
        finally:
            subprocess.run(["sudo","-n","umount",str(root)],check=True,timeout=10)


if __name__=="__main__":
    sys.exit(main())
