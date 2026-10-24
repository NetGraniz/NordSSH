"""Ephemeral loopback stalls for native cancellation and deadline assertions."""
import asyncio
import base64
import hashlib
import sys
import asyncssh


async def main():
    raw_connections = 0
    password_attempts = 0
    received = 0
    stall_auth = True
    auth_seen = asyncio.Event()

    async def silent(reader, writer):
        nonlocal raw_connections
        raw_connections += 1
        try:
            total = 0
            while data := await reader.read(4096):
                total += len(data)
                if total > 16384:
                    raise AssertionError("Silent fixture admission cap")
        finally:
            writer.close()
            await writer.wait_closed()

    class Server(asyncssh.SSHServer):
        def begin_auth(self, username):
            return True

        def password_auth_supported(self):
            return True

        async def validate_password(self, username, password):
            nonlocal password_attempts
            password_attempts += 1
            auth_seen.set()
            if stall_auth:
                await asyncio.sleep(10)
            return username == "fixture-user" and password == "synthetic-password"

    async def shell(process):
        nonlocal received
        process.stdout.write(b"ready\r\n")
        try:
            while data := await process.stdin.read(4096):
                received += len(data)
                if received > 4096:
                    raise AssertionError("Cancelled input admission cap")
        except asyncssh.Error:
            pass

    key = asyncssh.generate_private_key("ssh-ed25519")
    fingerprint = hashlib.sha256(base64.b64decode(key.export_public_key().split()[1])).hexdigest()
    listener = await asyncio.start_server(silent, "127.0.0.1", 0)
    server = await asyncssh.create_server(Server, "127.0.0.1", 0, server_host_keys=[key],
                                         process_factory=shell, encoding=None, login_timeout=15)
    try:
        modes = [
            ("invalid-address", 0, 0), ("pre-cancel-connect", 0, 0),
            ("past-deadline", 0, 0), ("far-deadline", 0, 0),
            ("cancel-connect", 1, 0), ("deadline-connect", 1, 0),
            ("pre-cancel-auth", 0, 0), ("pre-cancel-key", 0, 0),
            # An auth deadline may expire during service negotiation, before
            # the password packet. Both zero and one attempts are valid; the
            # coordinated cancel-auth case above requires the real stalled
            # password request to have reached the server before cancellation.
            ("cancel-auth", 0, 1), ("deadline-auth", 0, (0, 1)),
            ("cancel-read", 0, 1), ("pre-cancel-write", 0, 1),
        ]
        for mode, expected_tcp, expected_auth in modes:
            auth_seen.clear()
            stall_auth = mode not in ("cancel-read", "pre-cancel-write")
            use_silent = mode in ("invalid-address", "pre-cancel-connect", "past-deadline",
                                  "far-deadline", "cancel-connect", "deadline-connect")
            port = listener.sockets[0].getsockname()[1] if use_silent else server.get_port()
            before_tcp, before_auth = raw_connections, password_attempts
            child = await asyncio.create_subprocess_exec(sys.argv[1], mode, str(port), fingerprint, stdin=asyncio.subprocess.PIPE)
            try:
                if mode == "cancel-auth":
                    await asyncio.wait_for(auth_seen.wait(), 2)
                    child.stdin.write(b"C")
                    await child.stdin.drain()
                result = await asyncio.wait_for(child.wait(), 4)
            except BaseException:
                if child.returncode is None:
                    child.kill(); await child.wait()
                raise
            await asyncio.sleep(0.02)
            if result:
                raise AssertionError(f"Native cancellation fixture failed: {mode}")
            allowed_auth = expected_auth if isinstance(expected_auth, tuple) else (expected_auth,)
            if raw_connections - before_tcp != expected_tcp or password_attempts - before_auth not in allowed_auth:
                raise AssertionError(f"Unexpected network/authentication attempt: {mode}; "
                                     f"TCP {raw_connections-before_tcp}/{expected_tcp}, "
                                     f"auth {password_attempts-before_auth}/{expected_auth}")
        if received:
            raise AssertionError("Pre-cancelled write reached the server")
        print("12 native SSH cancellation/deadline modes passed")
    finally:
        listener.close(); await listener.wait_closed()
        server.close(); await server.wait_closed()


asyncio.run(main())
