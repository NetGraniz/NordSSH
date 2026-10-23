"""Print an independent, deterministic *synthetic* native-v1 test vector.

Build/test tool only, never shipped or used with real credentials. Requires
argon2-cffi 25.1.0 and cryptography. Writes no files and does not inspect vaults.
The fixed nonces and DEK below MUST NEVER be used by application code.
"""
import json
import struct
from importlib.metadata import version
from argon2.low_level import Type, hash_secret_raw
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

password = b"synthetic-password"
payload = b'{"schema":1,"hosts":[],"groups":[],"credentials":[],"known_hosts":[],"snippets":[],"settings":{}}'
header = bytearray(160)
header[:8] = b"NORDVLT\0"
struct.pack_into("<II", header, 8, 1, 160)
header[24:40] = bytes.fromhex("00112233445546778899aabbccddeeff")
struct.pack_into("<I", header, 40, 1)
header[44:60] = bytes(range(16))
header[60:72] = bytes(range(16, 28))
header[120:132] = bytes(range(32, 44))
struct.pack_into("<QQQ", header, 132, len(payload), 1, 1)
dek = bytes(range(48, 80))
kek = hash_secret_raw(password, bytes(header[44:60]), time_cost=3,
                      memory_cost=65536, parallelism=1, hash_len=32,
                      type=Type.ID, version=19)
header[72:120] = AESGCM(kek).encrypt(bytes(header[60:72]), dek,
                                   b"NordSSH-native-v1-wrap\0" + bytes(header[:60]))
ciphertext = AESGCM(dek).encrypt(bytes(header[120:132]), payload,
                               b"NordSSH-native-v1-data\0" + bytes(header))
print(json.dumps({"argon2_cffi": version("argon2-cffi"),
                  "argon2_cffi_bindings": version("argon2-cffi-bindings"),
                  "cryptography": version("cryptography"),
                  "kekHex": kek.hex(), "fileHex": (header+ciphertext).hex()}, indent=2))
