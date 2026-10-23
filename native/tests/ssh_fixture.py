"""Loopback-only SSH test server; synthetic password, ephemeral in-memory key.

Build/test dependency: AsyncSSH 2.24.1. No production hosts/accounts, filesystem
shell, external commands, system sshd service or firewall changes are involved.
"""
import asyncio
import base64
import hashlib
import sys
import asyncssh


async def main():
    terminal_gate = len(sys.argv) == 3 and sys.argv[2] == "--terminal"
    attempts = []
    received = bytearray()
    dimensions = []

    class Server(asyncssh.SSHServer):
        def begin_auth(self, username):
            return True

        def password_auth_supported(self):
            return True

        def validate_password(self, username, password):
            attempts.append(username)  # Never log/store the password.
            return username == "fixture-user" and password == "synthetic-ssh-password"

    async def shell(process):
        dimensions.append(process.get_terminal_size()[:2])
        process.stdout.write(b"fixture-ready\r\n")
        if terminal_gate:
            process.stdout.write(b"\x1b[?1049h\x1b[2J\x1b[Halternate-screen\x1b[?1049l")
        try:
            while True:
                try:
                    data = await process.stdin.read(65536)
                except asyncssh.TerminalSizeChanged as change:
                    dimensions.append((change.width, change.height))
                    continue
                if not data:
                    break
                received.extend(data)
                if len(received) > 65536:
                    raise AssertionError("Fixture input exceeded bound")
                process.stdout.write(b"echo:" + data)
        except asyncssh.Error:
            pass

    key = asyncssh.generate_private_key("ssh-ed25519")
    public = base64.b64decode(key.export_public_key().split()[1], validate=True)
    fingerprint = hashlib.sha256(public).hexdigest()
    server = await asyncssh.create_server(Server, "127.0.0.1", 0,
                                         server_host_keys=[key], process_factory=shell,
                                         encoding=None, login_timeout=10)
    try:
        modes = [("terminal", 1)] if terminal_gate else [
            ("untrusted", 0), ("changed", 0), ("replay", 0), ("wrong-password", 1), ("shell", 1)]
        for mode, expected_auth in modes:
            before = len(attempts)
            child = await asyncio.create_subprocess_exec(sys.argv[1], str(server.get_port()),
                                                        fingerprint, mode)
            try:
                result = await asyncio.wait_for(child.wait(), 12)
            except BaseException:
                if child.returncode is None:
                    child.kill()
                    await child.wait()
                raise
            if result:
                raise AssertionError(f"Native SSH fixture failed: {mode}")
            if len(attempts) - before != expected_auth:
                raise AssertionError(f"Unexpected authentication before approval: {mode}")
        expected = "once-привет" if terminal_gate else "once-привет\n"
        if bytes(received) != expected.encode("utf-8"):
            raise AssertionError("SSH input bytes were duplicated or changed")
        if not dimensions or dimensions[0] != (80, 24):
            raise AssertionError("PTY dimensions not received")
        if (100, 40) not in dimensions:
            raise AssertionError("PTY resize was not delivered to the server")
        print("Loopback SSH/terminal fixture passed; not a complete client gate")
    finally:
        server.close()
        await server.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())
