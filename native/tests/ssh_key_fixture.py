"""Ephemeral loopback key/user-certificate tests. Secrets travel only on stdin.

No key files, agents, external sshd, user SSH config or real hosts are used.
"""
import asyncio
import base64
import hashlib
import struct
import sys
import asyncssh


def mutate_key(encoded, change):
    raw = bytearray(base64.b64decode(b"".join(encoded.splitlines()[1:-1]), validate=True))
    change(raw)
    body = base64.b64encode(raw)
    return b"-----BEGIN OPENSSH PRIVATE KEY-----\n" + body + b"\n-----END OPENSSH PRIVATE KEY-----\n"


def options_offset(raw):
    offset = len(b"openssh-key-v1\0")
    for _ in range(2):
        size = struct.unpack_from(">I", raw, offset)[0]
        offset += 4 + size
    return offset


def rounds(raw, count):
    offset = options_offset(raw)
    size = struct.unpack_from(">I", raw, offset)[0]
    struct.pack_into(">I", raw, offset + size, count)


def replace_public(raw, public):
    offset = options_offset(raw)
    offset += 4 + struct.unpack_from(">I", raw, offset)[0] + 4
    size = struct.unpack_from(">I", raw, offset)[0]
    assert size == len(public)
    raw[offset + 4:offset + 4 + size] = public


async def main():
    host = asyncssh.generate_private_key("ssh-ed25519")
    ca = asyncssh.generate_private_key("ssh-ed25519")
    ed = asyncssh.generate_private_key("ssh-ed25519")
    ec = asyncssh.generate_private_key("ecdsa-sha2-nistp256")
    ec384 = asyncssh.generate_private_key("ecdsa-sha2-nistp384")
    ec521 = asyncssh.generate_private_key("ecdsa-sha2-nistp521")
    rsa = asyncssh.generate_private_key("ssh-rsa", key_size=2048)
    weak = asyncssh.generate_private_key("ssh-rsa", key_size=1024)
    other = asyncssh.generate_private_key("ssh-ed25519")
    phrase = b"synthetic-key-passphrase"
    encrypted = ed.export_private_key("openssh", passphrase=phrase, cipher_name="aes256-ctr", rounds=16, ignore_few_rounds=True)

    def cert(key, **options):
        return ca.generate_user_certificate(key, "synthetic", **options).export_certificate()

    accepted_keys = {key.export_public_key() for key in (ed, ec, ec384, ec521, rsa)}
    counts = dict(begins=0, checks=0, authenticated=0, shells=0)
    received = bytearray()

    class Server(asyncssh.SSHServer):
        def begin_auth(self, username):
            counts["begins"] += 1
            return True

        def public_key_auth_supported(self):
            return True

        def validate_public_key(self, username, key):
            counts["checks"] += 1
            return username == "fixture-user" and key.export_public_key() in accepted_keys

        def validate_ca_key(self, username, key):
            counts["checks"] += 1
            return username == "fixture-user" and key.export_public_key() == ca.export_public_key()

        def auth_completed(self):
            counts["authenticated"] += 1

    async def shell(process):
        counts["shells"] += 1
        try:
            data = await process.stdin.read(1024)
            received.extend(data)
            process.stdout.write(b"echo:" + data)
            await process.stdin.read(1)
        except asyncssh.Error:
            pass

    server = await asyncssh.create_server(Server, "127.0.0.1", 0, server_host_keys=[host],
        process_factory=shell, encoding=None, login_timeout=8,
        signature_algs=["ssh-ed25519", "ecdsa-sha2-nistp256", "rsa-sha2-512", "rsa-sha2-256"])
    fingerprint = hashlib.sha256(base64.b64decode(host.export_public_key().split()[1])).hexdigest()
    plain = ed.export_private_key("openssh")
    cases = [
        ("untrusted", plain, b"", b"", False, True),
        ("wrong-phrase", encrypted, b"wrong-synthetic-passphrase", b"", False, True),
        ("bad-format", b"not a private key", b"", b"", False, True),
        ("high-cost", mutate_key(encrypted, lambda raw: rounds(raw, 0xffffffff)), phrase, b"", False, True),
        ("reject-rounds-65", mutate_key(encrypted, lambda raw: rounds(raw, 65)), phrase, b"", False, True),
        ("reject-rounds-zero", mutate_key(encrypted, lambda raw: rounds(raw, 0)), phrase, b"", False, True),
        ("reject-nul", plain + b"\0", b"", b"", False, True),
        ("reject-phrase-nul", encrypted, phrase + b"\0suffix", b"", False, True),
        ("reject-public-mismatch", mutate_key(plain, lambda raw: replace_public(raw, base64.b64decode(other.export_public_key().split()[1]))), b"", b"", False, True),
        ("weak-rsa", weak.export_private_key("openssh"), b"", b"", False, True),
        ("plain-ed25519", plain, b"", b"", True, False),
        ("encrypted-ed25519", encrypted, phrase, b"", True, False),
        ("plain-ecdsa", ec.export_private_key("openssh"), b"", b"", True, False),
        ("plain-ecdsa384", ec384.export_private_key("openssh"), b"", b"", True, False),
        ("plain-ecdsa521", ec521.export_private_key("openssh"), b"", b"", True, False),
        ("plain-rsa", rsa.export_private_key("openssh"), b"", b"", True, False),
        ("encrypted-rsa", rsa.export_private_key("openssh", passphrase=phrase, cipher_name="aes256-ctr", rounds=16, ignore_few_rounds=True), phrase, b"", True, False),
        ("cert-ed25519", plain, b"", cert(ed, principals=["fixture-user"]), True, False),
        ("cert-rsa", rsa.export_private_key("openssh"), b"", cert(rsa, principals=["fixture-user"]), True, False),
        ("cert-ecdsa", ec.export_private_key("openssh"), b"", cert(ec, principals=["fixture-user"]), True, False),
        ("cert-mismatch", plain, b"", cert(other, principals=["fixture-user"]), False, True),
        ("expired", plain, b"", cert(ed, valid_before=1, principals=["fixture-user"]), False, False),
        ("principal", plain, b"", cert(ed, principals=["not-fixture-user"]), False, False),
        ("denied", other.export_private_key("openssh"), b"", b"", False, False),
    ]
    try:
        for mode, key, passphrase, certificate, success, local_reject in cases:
            before = counts.copy()
            child = await asyncio.create_subprocess_exec(sys.argv[1], str(server.get_port()), fingerprint, mode,
                stdin=asyncio.subprocess.PIPE)
            material = b"".join(struct.pack("<I", len(value)) + value for value in (key, passphrase, certificate))
            try:
                await asyncio.wait_for(child.communicate(material), 12)
            except BaseException:
                if child.returncode is None:
                    child.kill()
                    await child.wait()
                raise
            if child.returncode:
                raise AssertionError(f"Key fixture failed: {mode}")
            await asyncio.sleep(0)  # Let the fixture observe connection teardown.
            assert counts["authenticated"] - before["authenticated"] == int(success), f"Authentication state: {mode}"
            assert counts["shells"] - before["shells"] == int(success), f"Shell state: {mode}"
            if local_reject:
                assert counts["checks"] == before["checks"], f"Unexpected key authentication request: {mode}"
                assert counts["begins"] == before["begins"], f"Unexpected authentication start: {mode}"
            elif not success:
                assert counts["begins"] > before["begins"], f"Server did not exercise certificate/key rejection: {mode}"
        assert bytes(received) == b"key-auth-ok\n" * sum(case[4] for case in cases), "Key shell transport integrity"
        print(f"Loopback key gate: {len(cases)} cases; OpenSSH formats only, no UI/host-CA/2FA acceptance")
    finally:
        server.close()
        await server.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())
