"""Build-time only: acquire exact pinned SDK bytes before extraction."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

lock = json.loads((pathlib.Path(__file__).parent.parent / "qt-linux-engineering.lock.json").read_text())
destination = pathlib.Path(sys.argv[1]).resolve()
destination.mkdir(exist_ok=False, parents=True)
with tempfile.TemporaryDirectory(prefix="nordssh-qt-fetch-") as temp:
    for record in lock["archives"]:
        archive = pathlib.Path(temp) / record["file"]
        for mirror in lock["mirrors"]:
            try:
                with urllib.request.urlopen(mirror + record["file"], timeout=60) as response, archive.open("wb") as out:
                    total = 0
                    while chunk := response.read(1024 * 1024):
                        total += len(chunk)
                        if total > record["bytes"]:
                            raise RuntimeError("Qt archive exceeds pinned size")
                        out.write(chunk)
                break
            except (urllib.error.URLError, TimeoutError):
                if mirror == lock["mirrors"][-1]:
                    raise
        if archive.stat().st_size != record["bytes"]:
            raise RuntimeError("Qt archive size mismatch")
        with archive.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        if digest != record["sha256"]:
            raise RuntimeError("Qt SHA256 mismatch: refusing extraction or automatic pin change")
        target = destination / record["subdirectory"]
        target.mkdir(exist_ok=True)
        subprocess.run(["7z", "x", str(archive), f"-o{target}", "-y"], check=True, stdout=subprocess.DEVNULL)
        print(f"Verified/extracted Qt engineering archive: {record['file']}", flush=True)
