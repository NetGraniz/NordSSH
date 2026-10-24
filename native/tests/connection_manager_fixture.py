"""Actual async native controller against ephemeral loopback SSH only."""
import asyncio
import base64
import hashlib
import sys
import asyncssh


async def main():
    ui = len(sys.argv) == 3 and sys.argv[2] == "--ui"
    mode = ""
    attempts = []
    received = bytearray()
    dimensions = []
    errors = []
    connections = set()

    class Server(asyncssh.SSHServer):
        def connection_made(self, connection):
            connections.add(connection)

        def connection_lost(self, exc):
            pass

        def begin_auth(self, username):
            return True

        def password_auth_supported(self):
            return True

        def public_key_auth_supported(self):
            return True

        def validate_public_key(self, username, public_key):
            valid = username == "fixture-user" and public_key.export_public_key() == client_key.export_public_key()
            if valid:
                attempts.append(username)
            return valid

        async def validate_password(self, username, password):
            attempts.append(username)  # No retained/logged credential.
            if mode == "auth-timeout":
                await asyncio.sleep(2)
            return username == "fixture-user" and password == "synthetic-ssh-password"

    async def shell(process):
        case = mode
        dimensions.append(process.get_terminal_size()[:2])
        process.stdout.write(b"fixture-ready\r\n")
        try:
            if case == "flood-cancel":
                for _ in range(96):
                    process.stdout.write(b"O" * 65536)
                    await process.stdout.drain()
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
                    raise AssertionError("Unbounded fixture input")
                process.stdout.write(b"echo:" + data)
                if case in ("remote-eof", "ui-eof"):
                    if case == "ui-eof":
                        # More than one UI drain batch; finish with an unanswered
                        # VT query to test final output without a live peer.
                        process.stdout.write(b"\r\n" + b"tail-data\r\n" * 40000 + b"final-marker\r\n\x1b[6n")
                    await process.stdout.drain()
                    process.exit(0)
                    return
        except (asyncssh.Error, ConnectionError):
            pass
        except Exception as error:
            errors.append(str(error))

    async def silent(reader, writer):
        try:
            await asyncio.wait_for(reader.read(65536), 2)
            await asyncio.sleep(1)
        finally:
            writer.close()
            await writer.wait_closed()

    key = asyncssh.generate_private_key("ssh-ed25519")
    client_key = asyncssh.generate_private_key("ssh-ed25519")
    fingerprint = hashlib.sha256(base64.b64decode(key.export_public_key().split()[1])).hexdigest()
    server = await asyncssh.create_server(Server, "127.0.0.1", 0, server_host_keys=[key],
                                         process_factory=shell, encoding=None, login_timeout=10)
    banner = await asyncio.start_server(silent, "127.0.0.1", 0)
    try:
        modes = ("ui-password", "ui-reject", "ui-changed", "ui-eof") if ui else (
            "capacity", "cancel-dns", "destroy-banner", "untrusted", "bad-challenge", "changed",
            "destroy-trust", "trust-timeout", "wrong-password", "auth-timeout", "password", "cached", "key", "shutdown", "reconnect", "remote-eof", "flood-cancel")
        for mode in modes:
            received.clear()
            dimensions.clear()
            before = len(attempts)
            port = banner.sockets[0].getsockname()[1] if mode == "destroy-banner" else server.get_port()
            child = await asyncio.create_subprocess_exec(sys.argv[1], str(port), fingerprint, mode,
                stdin=asyncio.subprocess.PIPE if mode == "key" else None)
            try:
                if mode == "key":
                    payload = client_key.export_private_key("openssh", "synthetic-key-passphrase",
                        cipher_name="aes256-ctr", rounds=16, ignore_few_rounds=True)
                    child.stdin.write(len(payload).to_bytes(4, "big") + payload)
                    await child.stdin.drain()
                    child.stdin.close()
                result = await asyncio.wait_for(child.wait(), 15)
                if result:
                    raise AssertionError(f"Async manager failed: {mode}")
            finally:
                if child.returncode is None:
                    child.kill()
                    await child.wait()
            count = len(attempts) - before
            expected = 1 if mode in ("password", "cached", "wrong-password", "shutdown", "ui-password", "ui-eof", "key", "reconnect", "remote-eof", "flood-cancel") else 0
            if mode == "auth-timeout":
                if count not in (0, 1):
                    raise AssertionError("Duplicated stalled authentication")
            elif count != expected:
                raise AssertionError(f"Unexpected credential transmission: {mode}: {count}")
            if mode in ("password", "cached", "shutdown", "ui-password", "ui-eof", "key", "reconnect", "remote-eof"):
                expected_input = "once-привет\r" if ui else "once-привет\n"
                if bytes(received) != expected_input.encode():
                    raise AssertionError(f"Synthetic input differs: {bytes(received)!r}")
                if not ui and (not dimensions or dimensions[0] != (100, 40)):
                    raise AssertionError("Latest dimensions not used at shell open")
            elif received:
                raise AssertionError("Input leaked before an active trusted shell")
            for connection in connections:
                connection.close()
            await asyncio.gather(*(c.wait_closed() for c in connections), return_exceptions=True)
            connections.clear()
        if errors:
            raise AssertionError(errors)
        print(f"{len(modes)} native connection modes passed; not full application acceptance")
    finally:
        server.close()
        await server.wait_closed()
        banner.close()
        await banner.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())
