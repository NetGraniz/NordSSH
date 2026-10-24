"""Two ephemeral SSH identities; forwarding restricted to loopback fixtures."""
import asyncio
import base64
import hashlib
import sys
import asyncssh


async def main():
    attempts = {"jump": 0, "dest": 0}
    connections = []

    async def echo(reader, writer):
        try:
            while data := await reader.read(65536):
                writer.write(data)
                await writer.drain()
            writer.write(b"tail-after-eof")
            await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()

    echo_server = await asyncio.start_server(echo,"127.0.0.1",0)
    echo_port = echo_server.sockets[0].getsockname()[1]

    class Server(asyncssh.SSHServer):
        def __init__(self, role):
            self.role = role
        def connection_made(self, connection):
            self.connection = connection
            connections.append(connection)
        def begin_auth(self, username):
            return True
        def password_auth_supported(self):
            return True
        def validate_password(self, username, password):
            attempts[self.role] += 1
            return username == self.role + "-user" and password == self.role + "-synthetic-password"
        async def connection_requested(self, host, port, _origin, _origin_port):
            if self.role != "jump" or (host,port) not in (("127.0.0.1",echo_port),("destination.invalid",target.get_port())):
                return False
            return await self.connection.forward_connection("127.0.0.1",port)

    async def shell(process):
        process.stdout.write(b"destination-ready\n")
        try:
            while data := await process.stdin.read(65536):
                process.stdout.write(b"echo:"+data)
        except asyncssh.Error:
            pass

    def identity():
        key = asyncssh.generate_private_key("ssh-ed25519")
        return key, hashlib.sha256(base64.b64decode(key.export_public_key().split()[1])).hexdigest()

    jump_key,jump_hash = identity()
    target_key,target_hash = identity()
    target = await asyncssh.create_server(lambda:Server("dest"),"127.0.0.1",0,server_host_keys=[target_key],process_factory=shell,encoding=None)
    jump = await asyncssh.create_server(lambda:Server("jump"),"127.0.0.1",0,server_host_keys=[jump_key],encoding=None)
    try:
        for mode in ("forward","jump","reject-jump","changed-target"):
            before = attempts.copy()
            child = await asyncio.create_subprocess_exec(sys.argv[1],str(jump.get_port()),jump_hash,str(target.get_port()),target_hash,str(echo_port),mode)
            try:
                code = await asyncio.wait_for(child.wait(),25)
            except BaseException:
                if child.returncode is None:
                    child.kill()
                    await child.wait()
                raise
            if code:
                raise AssertionError(f"Native forwarding test failed: {mode}")
            expected = (0,0) if mode=="reject-jump" else (1,1) if mode=="jump" else (1,0)
            if (attempts["jump"]-before["jump"],attempts["dest"]-before["dest"]) != expected:
                raise AssertionError("Authentication occurred before the correct host approval")
        print("Concurrent forwarding, TCP half-close, jump trust and changed/rejected key gates passed")
    finally:
        for connection in connections:
            connection.close()
        await asyncio.gather(*(connection.wait_closed() for connection in connections),return_exceptions=True)
        jump.close()
        target.close()
        echo_server.close()
        await asyncio.gather(jump.wait_closed(),target.wait_closed(),echo_server.wait_closed())


if __name__ == "__main__":
    asyncio.run(main())
