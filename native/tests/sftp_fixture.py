"""Ephemeral loopback SFTP root, keys and synthetic credentials only."""
import asyncio
import base64
import hashlib
import sys
import tempfile
from pathlib import Path
import asyncssh
from asyncssh.sftp import SFTPServerHandler


async def main():
    connections = []
    class Server(asyncssh.SSHServer):
        def connection_made(self, connection):
            connections.append(connection)
        def begin_auth(self, username):
            return True
        def password_auth_supported(self):
            return True
        def validate_password(self, username, password):
            return username == "fixture-user" and password == "synthetic-ssh-password"

    key = asyncssh.generate_private_key("ssh-ed25519")
    fingerprint = hashlib.sha256(base64.b64decode(key.export_public_key().split()[1])).hexdigest()
    for mode in ("basic", "cancel-upload", "cancel-download", "timeout", "no-extension", "commit-unknown", "oversize", "queue"):
        with tempfile.TemporaryDirectory(prefix="nordssh-sftp-") as directory:
            stalled = set()
            opened = set()
            root = Path(directory)
            (root / "existing").write_bytes(b"remote-original")
            (root / "stall").write_bytes(b"stalled-file")

            class Sftp(asyncssh.SFTPServer):
                def __init__(self, channel):
                    super().__init__(channel, chroot=directory)
                def lstat(self, path):
                    attrs = super().lstat(path)
                    if mode == "oversize" and path == b"/existing":
                        attrs = asyncssh.SFTPAttrs.from_local(attrs)
                        attrs.size = (1 << 40) + 1
                    return attrs
                def posix_rename(self, oldpath, newpath):
                    result = super().posix_rename(oldpath, newpath)
                    if mode == "commit-unknown":
                        self.connection.abort()
                        raise asyncssh.SFTPFailure("Synthetic lost commit acknowledgement")
                    return result
                def open(self, path, pflags, attrs):
                    result = super().open(path, pflags, attrs)
                    opened.add(result)
                    return result
                def close(self, file_obj):
                    opened.discard(file_obj)
                    return super().close(file_obj)
                async def read(self, file_obj, offset, size):
                    if str(file_obj.name).endswith("stall") or str(file_obj.name).endswith("stall'"):
                        task = asyncio.current_task()
                        stalled.add(task)
                        try:
                            await asyncio.sleep(30)
                        finally:
                            stalled.discard(task)
                    return super().read(file_obj, offset, size)

            original_extensions = SFTPServerHandler._extensions
            if mode == "no-extension":
                # Exact-pin test-only advertisement hook: reject BEFORE upload,
                # rather than merely returning a rename error after staging.
                SFTPServerHandler._extensions = [item for item in original_extensions
                    if item[0] not in (b"posix-rename@openssh.com", b"hardlink@openssh.com")]
            server = await asyncssh.create_server(Server,"127.0.0.1",0,server_host_keys=[key],
                sftp_factory=Sftp,encoding=None)
            try:
                child = await asyncio.create_subprocess_exec(sys.argv[1],str(server.get_port()),fingerprint,mode)
                try:
                    result = await asyncio.wait_for(child.wait(),25)
                except BaseException:
                    if child.returncode is None:
                        child.kill()
                        await child.wait()
                    raise
                if result:
                    raise AssertionError(f"Native SFTP test failed: {mode}")
                expected = b"u" * 262144 if mode in ("basic", "commit-unknown") else b"remote-original"
                if (root / "existing").read_bytes() != expected:
                    raise AssertionError("Original target was damaged")
                if mode == "no-extension" and any(root.glob(".nordssh-*.part")):
                    raise AssertionError("Unsupported overwrite was staged before capability admission")
            finally:
                SFTPServerHandler._extensions = original_extensions
                for task in tuple(stalled):
                    task.cancel()
                if stalled:
                    await asyncio.gather(*tuple(stalled), return_exceptions=True)
                for connection in connections:
                    connection.close()
                await asyncio.gather(*(connection.wait_closed() for connection in connections), return_exceptions=True)
                connections.clear()
                for file_obj in opened:
                    file_obj.close()
                opened.clear()
                server.close()
                await server.wait_closed()
    print("SFTP CRUD, transfer integrity, queue/tree, overwrite, timeout and cancel fixture passed")


if __name__ == "__main__":
    asyncio.run(main())
