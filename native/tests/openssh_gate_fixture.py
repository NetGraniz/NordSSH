"""Real sshd only on disposable GitHub runners, never the user's SSH service.

Creates one unique test account with password login disabled in a private sshd
config, synthetic keys and loopback listeners. No system sshd config/host key is
read or modified. The account, daemon and temporary directory are removed at exit.
"""
import asyncio
import base64
import hashlib
import os
import pwd
import signal
import socket
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
import asyncssh


def sudo(*args, **kwargs):
    return subprocess.run(["sudo","-n",*map(str,args)],check=True,timeout=10,**kwargs)


async def main():
    if os.environ.get("NORDSSH_ISOLATED_OPENSSH")!="1" or os.environ.get("GITHUB_ACTIONS")!="true":
        raise RuntimeError("Real sshd fixture is restricted to explicitly enabled disposable CI runners")

    async def echo(reader,writer):
        try:
            while data:=await reader.read(65536):
                writer.write(data)
                await writer.drain()
            writer.write(b"tail-after-eof")
            await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()

    echo_server=await asyncio.start_server(echo,"127.0.0.1",0)
    echo_port=echo_server.sockets[0].getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="nordssh-openssh-") as directory:
        root=Path(directory).resolve()
        if root.parent!=Path(tempfile.gettempdir()).resolve() or not root.name.startswith("nordssh-openssh-") or root.is_symlink():
            raise RuntimeError("Unexpected fixture root")
        root.chmod(0o755)
        user="nssh"+uuid.uuid4().hex[:12]
        created=False
        daemon=None
        log=None
        try:
            remote=root/"remote"
            remote.mkdir(mode=0o700)
            sudo("useradd","--no-create-home","--home-dir",remote,"--shell","/bin/sh","--password","",user)
            created=True
            account=pwd.getpwnam(user)
            work=remote/"work"
            work.mkdir(mode=0o700)
            original=work/"existing"
            original.write_bytes(b"real-openssh-original")
            original.chmod(0o600)
            (work/"linked").symlink_to(original)
            client=asyncssh.generate_private_key("ssh-ed25519")
            host=asyncssh.generate_private_key("ssh-ed25519")
            host_file=root/"host-key"
            host_file.write_bytes(host.export_private_key("openssh"))
            host_file.chmod(0o600)
            authorized=remote/"authorized_keys"
            authorized.write_bytes(client.export_public_key())
            authorized.chmod(0o600)
            for path in (original,authorized,work,remote):
                sudo("chown",f"{account.pw_uid}:{account.pw_gid}",path)
            # AuthorizedKeysFile is inside the private test home: StrictModes
            # stops there, rather than considering the shared /tmp ancestor.
            with socket.socket() as reserved:
                reserved.bind(("127.0.0.1",0))
                port=reserved.getsockname()[1]
            config=root/"sshd_config"
            config.write_text(f"""Port {port}
ListenAddress 127.0.0.1
AddressFamily inet
HostKey {host_file}
PidFile {root/'sshd.pid'}
AuthorizedKeysFile {authorized}
AllowUsers {user}
AuthenticationMethods publickey
PubkeyAuthentication yes
PasswordAuthentication no
PermitEmptyPasswords no
KbdInteractiveAuthentication no
UsePAM no
PermitRootLogin no
StrictModes yes
LoginGraceTime 10
MaxAuthTries 3
MaxSessions 4
MaxStartups 4
UseDNS no
PrintMotd no
X11Forwarding no
AllowAgentForwarding no
AllowTcpForwarding local
PermitOpen 127.0.0.1:{echo_port}
GatewayPorts no
Subsystem sftp internal-sftp
LogLevel ERROR
""",encoding="utf-8")
            sudo("mkdir","-p","/run/sshd")
            sudo("/usr/sbin/sshd","-t","-f",config)
            log=(root/"daemon.log").open("wb")
            sudo("chown","root:root",root)
            daemon=subprocess.Popen(["sudo","-n","/usr/sbin/sshd","-D","-e","-f",str(config)],
                stdout=subprocess.DEVNULL,stderr=log,start_new_session=True)
            for _ in range(100):
                if daemon.poll() is not None:
                    raise RuntimeError("Private sshd stopped")
                try:
                    reader,writer=await asyncio.open_connection("127.0.0.1",port)
                    banner=await asyncio.wait_for(reader.readline(),1)
                    writer.close()
                    await writer.wait_closed()
                    if banner.startswith(b"SSH-2.0-OpenSSH"):
                        break
                except (OSError,asyncio.TimeoutError):
                    await asyncio.sleep(0.05)
            else:
                raise RuntimeError("Private sshd unavailable")
            fingerprint=hashlib.sha256(base64.b64decode(host.export_public_key().split()[1])).hexdigest()
            for mode in ("basic","badkey","untrusted"):
                key=asyncssh.generate_private_key("ssh-ed25519") if mode=="badkey" else client
                child=await asyncio.create_subprocess_exec(sys.argv[1],str(port),fingerprint,user,str(work),str(echo_port),mode,stdin=asyncio.subprocess.PIPE)
                try:
                    await asyncio.wait_for(child.communicate(key.export_private_key("openssh")),25)
                except BaseException:
                    if child.returncode is None:
                        child.kill()
                        await child.wait()
                    raise
                if child.returncode:
                    raise RuntimeError(f"Real OpenSSH client gate failed: {mode}")
            print("Real private OpenSSH public-key/SFTP CRUD/staging/overwrite/symlink/forwarding and negative auth passed")
        finally:
            if daemon is not None and daemon.poll() is None:
                sudo("/bin/kill","-TERM","--",f"-{daemon.pid}")
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    sudo("/bin/kill","-KILL","--",f"-{daemon.pid}")
                    daemon.wait(timeout=5)
            if log:
                log.close()
            if created:
                subprocess.run(["sudo","-n","pkill","-TERM","-u",str(account.pw_uid)],check=False,timeout=5)
                sudo("userdel",user)
            # Exact validated fresh temporary root; GNU chown -P does not follow
            # recursive symlinks. This is not a user's home or a workspace root.
            sudo("chown","-R","-P",f"{os.getuid()}:{os.getgid()}",root)
    echo_server.close()
    await echo_server.wait_closed()


if __name__=="__main__":
    asyncio.run(main())
