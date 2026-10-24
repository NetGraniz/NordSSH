"""Disposable loopback SSH flood/duplex fixture; no real shell or credentials."""
import asyncio
import base64
import hashlib
import sys
import asyncssh


async def main():
    mode = ""
    errors = []
    completed = []

    class Server(asyncssh.SSHServer):
        def begin_auth(self, username):
            return True

        def password_auth_supported(self):
            return True

        def validate_password(self, username, password):
            return username == "fixture-user" and password == "synthetic-ssh-password"

    async def shell(process):
        case = mode
        try:
            process.stderr.write(b"E")
            # Output exceeds both the app queue and usual SSH channel window.
            # Do not read input until drain completes: a write-only pump deadlocks.
            for _ in range(48):
                process.stdout.write(b"O" * 65536)
                await process.stdout.drain()
            total = 0
            while total < 2 * 1024 * 1024:
                data = await process.stdin.read(65536)
                if not data:
                    raise AssertionError("Unexpected client EOF")
                if data != b"I" * len(data):
                    raise AssertionError("Input duplicated, reordered or changed")
                total += len(data)
            if total != 2 * 1024 * 1024:
                raise AssertionError("Input length exceeded expected bytes")
            process.stdout.write(b"pump-ok\n")
            await process.stdout.drain()
            completed.append(case)
            process.exit(0)
        except (asyncssh.Error, BrokenPipeError, ConnectionError):
            if case != "cancel":
                errors.append("Unexpected duplex disconnect")
        except Exception as error:
            errors.append(str(error))

    key = asyncssh.generate_private_key("ssh-ed25519")
    fingerprint = hashlib.sha256(base64.b64decode(key.export_public_key().split()[1])).hexdigest()
    server = await asyncssh.create_server(Server, "127.0.0.1", 0, server_host_keys=[key],
                                         process_factory=shell, encoding=None, login_timeout=10)
    try:
        for mode in ("duplex", "cancel"):
            child = await asyncio.create_subprocess_exec(sys.argv[1], str(server.get_port()), fingerprint, mode)
            try:
                result = await asyncio.wait_for(child.wait(), 18)
                if result:
                    raise AssertionError(f"Shell pump failed: {mode}")
            finally:
                if child.returncode is None:
                    child.kill()
                    await child.wait()
        if errors or completed != ["duplex"]:
            raise AssertionError(f"Fixture verification failed: {errors}, {completed}")
        print("Bounded shell pump fixture passed; not a GUI/session-manager gate")
    finally:
        server.close()
        await server.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())
