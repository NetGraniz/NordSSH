# Native NordSSH vault format v1

Status: normative design for the new native client, not a claim of implemented
encryption. Date: 2026-10-23. Read with NATIVE_REQUIREMENTS.md and ADR 0001.

This format is unrelated to the Tauri prototype. Implementations MUST NOT inspect,
auto-migrate, overwrite or delete prototype vaults, backups or signing keys.
Native application data uses a separate namespace. A file with a different magic
or version is unsupported input, never an empty/new vault.

## 1. Primitives and limits

MUST/MUST NOT define acceptance rules. Sizes below are bytes unless stated.

| Item | v1 requirement |
| --- | --- |
| Payload and key wrapping | AES-256-GCM through OpenSSL EVP; no alternate cipher |
| Data-encryption key (DEK) | 32 independently random bytes per vault |
| Password key (KEK) | 32-byte Argon2id output, version 0x13 |
| KDF profile 1 | memory_cost=65536 KiB, time_cost=3, parallelism=1, output_length=32 |
| Argon2 optional secret/associated data | Both absent; not the GCM AAD below |
| Password salt | 16 random bytes |
| GCM nonces | Two independently generated 12-byte values |
| GCM tags | Exactly 16 bytes; truncation forbidden |
| Header | Exactly 160 bytes |
| Plaintext payload | 1..8388608 bytes, strict UTF-8 JSON |
| File length | Exactly 160 + ciphertext_length + 16; maximum 8388784 |
| Password | At least 7 Unicode scalar values and at most 4096 UTF-8 bytes |

KDF profile 1 is this application's selected 64 MiB/three-pass/one-lane profile.
It is NOT the complete second recommended RFC 9106 profile, which uses four lanes.
All profile values are fixed, not values supplied by imported input. A future KDF
profile requires a reviewed format/spec change, not an automatic stronger/weaker
fallback. Reject unknown profiles before allocation or password derivation.

Generate UUID, DEK, salt and nonces with an OS-backed CSPRNG, checking every
OpenSSL RAND_bytes/RAND_priv_bytes return value. RNG failure aborts creation/save.
Do not seed a custom PRNG. DEK, KEK, plaintext, passwords and scratch buffers have
bounded lifetimes and best-effort explicit cleansing on success and every error.
Do not log these values, ciphertext/plaintext dumps or KDF inputs. Ciphertext may
remain in backups; it is not a safe substitute for deleting an exposed password.

## 2. File layout

The file is binary. Every multi-byte integer is unsigned little-endian, decoded
with explicit byte operations, never a packed C++ struct or host-endian cast.
There is no BOM, JSON envelope, base64, compression, padding or trailing data.

| Offset | Length | Field | Accepted value/encoding |
| --- | --- | --- | --- |
| 0 | 8 | magic | Hex 4e 4f 52 44 56 4c 54 00, ASCII `NORDVLT` plus NUL |
| 8 | 4 | format_version | 1 |
| 12 | 4 | header_length | 160 |
| 16 | 4 | flags | 0; every bit reserved |
| 20 | 4 | reserved_a | All zero |
| 24 | 16 | vault_uuid | RFC 9562 UUIDv4 bytes in network/textual UUID order, not Windows GUID memory order |
| 40 | 4 | kdf_profile | 1 |
| 44 | 16 | salt | Random password salt |
| 60 | 12 | wrap_nonce | AES-GCM KEK nonce |
| 72 | 32 | wrapped_dek | AES-GCM ciphertext of exactly the 32-byte DEK |
| 104 | 16 | wrap_tag | Full wrapping authentication tag |
| 120 | 12 | data_nonce | AES-GCM DEK nonce |
| 132 | 8 | ciphertext_length | 1..8388608; equal to plaintext length for GCM |
| 140 | 8 | revision | 1..9223372036854775807 |
| 148 | 8 | key_revision | Revision when the current DEK was created; 1..revision |
| 156 | 4 | reserved_b | All zero |
| 160 | ciphertext_length | ciphertext | Encrypted JSON payload |
| 160 + ciphertext_length | 16 | data_tag | Full payload authentication tag |

Reject invalid UUID variant/version, nonzero reserved fields, unknown version,
flags/profile, truncated files, excess bytes and integer overflow. Verify bounds
before adding lengths, reading the body or allocating buffers. Header length is
not an invitation to accept extension bytes. Do not accept a JSON prototype file
that happens to contain a `format: 1` field.

## 3. Exact authenticated data

`||` concatenates byte sequences; `[a:b]` is zero-based, end-exclusive. The two
domain separators contain only the following ASCII bytes and one final NUL:

```text
WRAP_DOMAIN = "NordSSH-native-v1-wrap\0"
DATA_DOMAIN = "NordSSH-native-v1-data\0"

wrap_aad = WRAP_DOMAIN || header[0:60]
data_aad = DATA_DOMAIN || header[0:160]
```

Each domain is 23 bytes: 22 visible ASCII characters plus NUL. Do not include C
string terminators twice or use QString UTF-16 bytes. Test exact domain bytes and
AAD byte lengths (83 for wrap, 183 for data) in both supported builds.

Header[0:60] includes magic/version, flags, UUID, KDF profile and salt. It is
immutable while a given wrapped DEK is reused. GCM itself binds wrap_nonce,
wrapped_dek and wrap_tag. The payload tag additionally authenticates the ENTIRE
header, including the wrapping tuple, payload nonce/length, revision and reserved
fields. This catches wrapper tampering even during remembered-device unlock.

No JSON serialization or canonicalization participates in AAD. An implementation
must feed the exact stored header bytes to verification, not reconstructed values
whose representation could differ.

## 4. Creation, decryption and saving

Creation:

1. Validate password, plaintext size and JSON schema before committing a file.
2. Generate UUIDv4, salt, DEK, wrap_nonce and data_nonce. Set revision=key_revision=1.
3. Derive KEK with the exact profile. Encrypt DEK using wrap_aad; write its
   ciphertext and tag into the header.
4. Encrypt JSON bytes with DEK, data_nonce and data_aad; append ciphertext/tag.
5. Atomically commit the complete encrypted file under section 8. Save a device
   key only after successful commit and only with explicit user preference.

Password opening:

1. Bounded read and validate the fixed header/layout before running Argon2.
2. Validate password bytes, derive KEK and decrypt the wrapping tuple.
3. Accept the DEK only after successful GCM authentication and exact 32-byte output.
4. Decrypt the payload with the authenticated DEK. EVP_DecryptUpdate can produce
   tentative plaintext before final verification: keep it private, do not parse,
   display, signal or act on it until EVP_DecryptFinal_ex reports success.
5. Verify strict UTF-8/JSON/schema limits. Only then publish a new unlocked model.

Set the expected 16-byte tag before EVP_DecryptFinal_ex. Check every EVP call and
length result. Authentication failure clears tentative plaintext and leaves the
on-disk file and previous unlocked model unchanged. Error text may say wrong
password or damaged vault; do not infer which from unauthenticated contents.

Ordinary save reuses the existing DEK and wrapping tuple, increments revision and
generates a fresh data_nonce. It MUST NOT derive a new KEK from an empty password,
require retaining the user's password, or reuse the prior payload nonce. Revision
overflow fails closed. Set key_revision=revision when creating/rotating a DEK.
Reject revision-key_revision >=1048576 on read. Before a save would reach that
difference, require full DEK/salt/wrapper rotation and set key_revision to the
new revision. This limits a DEK to 1048576 committed revisions without confusing
the lifetime counter with the vault's monotonically increasing revision. The UI
may request a password for this rare rotation. No plaintext fallback.

Creating an independent export or changing the vault password generates a new
DEK, salt and both nonces, then re-encrypts the payload. Independent export also
gets a new vault UUID and revision=key_revision=1; password change keeps the
logical UUID and advances revision, setting key_revision to that new revision.
Keep only encrypted backups. Device unlock records are
updated/deleted after a successful password-change commit; any failure is surfaced
as a device-key cleanup error, not reported as a failed password change after it
has already committed. Old data/backup keys cannot be remotely revoked.

## 5. Password byte contract

Convert valid Unicode to UTF-8 exactly once, with strict rejection of isolated
UTF-16 surrogates/invalid UTF-8. Count Unicode scalar values AFTER validation;
neither UTF-16 code units, bytes nor grapheme clusters define the seven-symbol
minimum. An astral character counts as one; a combining mark is a separate scalar.
Show this policy consistently in creation/change/export UI.

Reject more than 4096 UTF-8 bytes BEFORE KDF allocation. Apply the same 7-scalar
minimum to create/change/open/export for native v1; no legacy shorter passwords
exist in this format. Do not trim spaces/newlines, case-fold, normalize NFC/NFKC,
or silently replace characters. Repeat-password comparison uses exact UTF-8 bytes.
Embedded NUL is representable at the format level: pass explicit byte lengths,
never strlen; if a UI cannot represent it, do not silently truncate it.

One KDF at a time per vault and at most two process-wide prevents user-triggered
parallel derivations from exhausting RAM. KDF runs off the GUI thread. Cancellation
does not use QThread::terminate; let a noninterruptible derivation finish, wipe its
result and ignore it using the cancelled request ID. Rate limiting improves UX,
not offline password security. Seven characters is an accepted product minimum,
not a guarantee against offline guessing of a stolen file.

## 6. Plaintext JSON contract and parser limits

The authenticated plaintext is strict UTF-8 RFC 8259 JSON, no BOM/comments/trailing
commas/trailing tokens. Reject duplicate object names at every depth BEFORE an
ordinary DOM parser can discard duplicates. Reject invalid escapes, unpaired
surrogates, embedded unescaped controls, NaN/Infinity and out-of-range numbers.

Root is an object with exactly these keys:

```json
{"schema":1,"hosts":[],"groups":[],"credentials":[],"known_hosts":[],"snippets":[],"settings":{}}
```

The five arrays contain objects only; settings is an object. Each product module
must define and validate its records, IDs, references and allowed settings before
using them. This document fixes the encryption/container contract, not permission
to accept unvalidated opaque records. A separate reviewed record-schema contract
is required before persisting nonempty production data. Unknown root fields and
unsupported schema versions fail closed; no silent field dropping on save.

| Parser/resource bound | Maximum |
| --- | --- |
| Plaintext bytes before JSON parsing | 8388608 |
| Container nesting, root depth=1 | 16 |
| Total JSON nodes, including containers and scalar values | 100000 |
| Members of any one object | 256 |
| Elements of any one array | 10000 |
| Object key after UTF-8 decoding | 128 bytes |
| String value after UTF-8 decoding | 1048576 bytes |
| Numeric token | 20 ASCII bytes, integer syntax only, magnitude <=9007199254740991 |

Payload schema 1 uses integers, not floating-point/exponent notation. IDs and
64-bit opaque identifiers outside this range are strings, with module-specific
validation. Bounds are checked during a bounded pre-scan/tokenization, not after
building an unbounded QJsonDocument. Encrypted private-key/certificate strings are
subject to the same one-MiB bound. Imported data never invokes shell commands,
auto-connects to hosts or changes application/update policy during parsing.

## 7. Remembered device unlock and import

DPAPI user scope / Linux Secret Service may store the raw DEK only inside their
protected credential mechanism, indexed by the native product namespace + UUID.
Never use machine-wide DPAPI or plaintext beside portable data. A missing/locked
backend falls back to password opening, without replacing any vault.

Remembered opening still validates ALL layout/JSON limits and the payload tag
over the complete header. Never skip AEAD because a trusted device key exists.
Device records are not exported. Full lock wipes live secrets after the user has
resolved active sessions/transfers; retained remembered keys remain a separate
explicit preference. Forgetting a record cannot revoke an already stolen DEK.

Import decrypts and validates a candidate in isolation. No implicit merge. A
replacement requires confirmation and an encrypted backup of the exact previous
file, followed by section 8 commit. Cancellation or invalid input preserves the
current file/model. Importing as an independent vault re-encrypts with a fresh
UUID/DEK/salt; do not create duplicate active UUIDs/device-key bindings silently.
Export to the active file or its filesystem alias is rejected.

## 8. Persistence and concurrency

Require a regular file on a supported local filesystem. Reject symlink/reparse
targets, special devices and unexpected target identity changes; do not silently
follow links while creating vaults/backups. Use owner-only file permissions on
Unix and a current-user ACL on Windows. If the destination filesystem cannot
enforce the selected permission/atomicity policy, report it rather than claiming
those guarantees. Do not change ACLs/permissions on unrelated parent directories.

Keep one worker-owner for each unlocked vault and a process-wide registry keyed
by normalized file identity/path. Take an exclusive QLockFile for writing using
one agreed canonical sidecar path. Disable automatic time-based stale-lock
takeover (setStaleLockTime(0)); no manual removeStaleLockFile during a pending
operation. Qt's verified dead-process detection is allowed; a manual stale-lock
recovery flow must verify ownership/dead-process state and obtain explicit
confirmation, not blindly remove a live lock. The lock is advisory
between cooperating instances, not protection from same-user malware.

On opening retain the exact encrypted-file SHA-256 and filesystem identity as the
expected snapshot. Under the write lock, re-read the bounded current file and
compare its digest, identity and revision to the snapshot before every save.
File absence is a distinct expected state for creation. Disappearance, external
replacement, conflicting revision or content change rejects the save; do not
silently overwrite/reload and lose in-memory edits. Recheck immediately before
commit. Network filesystem/hostile uncooperative-writer TOCTOU guarantees are not
claimed; such locations are not supported for concurrent writable vaults.

Serialize and encrypt a candidate before changing the live model. Use QSaveFile
in the destination directory with setDirectWriteFallback(false); write the full
encrypted file, apply permissions, check every write and commit() result. Never
write plaintext to a temporary file. Only after successful commit update the
live model/digest/revision. Temporary files and encrypted backups receive the same
permission policy; backup failure prevents a requested destructive import.

QSaveFile provides atomic replacement where supported, not a universal guarantee
against storage failure/power loss. Durable-save claims require platform-specific
flush/fsync and directory-metadata tests. An I/O error leaves the original file
unchanged or a recoverable encrypted backup; UI does not report unsaved work as
committed. A crash between file commit and UI refresh is recovered by re-opening
the committed file, not replaying the stale in-memory save.

Revision and digest detect local stale writes. They do not cryptographically
prevent an attacker replacing BOTH the complete file and its old encrypted
backup with an earlier valid revision. Offline rollback protection would require
an external trusted monotonic record and is not promised by portable v1.

## 9. Required evidence before a native vault release

- AES-256-GCM known-answer vectors, including nonempty AAD; official Argon2id
  vectors; fixed application-profile vectors produced by independent reference
  tools. Test the composition, not just OpenSSL/library return values.
- Exact little-endian/header/domain/AAD fixtures. Windows-written files open on
  Linux and vice versa with identical decoded records; never use actual secrets.
- Password lengths 6/7, spaces, Cyrillic, astral/combining characters, embedded
  NUL, invalid surrogates/UTF-8, 4096-byte boundary and mismatched confirmation.
- Flip EACH header field/wrapper/data/tag, wrong password/device key, splice a
  wrapper/payload from another vault, reserved bits, UUID/profile mismatch,
  unsupported version, huge/overflowed length, truncation and appended bytes.
  Remembered opening MUST detect wrapping metadata changes too.
- Duplicate JSON keys, malformed UTF-8/escapes, nested/large objects, huge strings,
  float/exponent/overflowed numbers, unknown schema/fields, broken references and
  total-limit boundaries. No side effects occur before complete verification.
- Simulated RNG/Argon2/EVP failures and cancellation: no partial model/file,
  plaintext log, retry with weak KDF, reused nonce or forgotten cleanup.
- Fresh nonce on every save; write rotation bound, password-change/export
  independent DEK, stale remembered key failure and preserved old encrypted data.
- Two cooperating processes, external replace/delete, symlink/reparse input,
  export alias, permission failure, read-only/disk-full destinations, failed
  temp write/rename/backup, commit success followed by UI crash and stale lock.
- Import/lock/update/vault-switch with active SSH/SFTP obey the activity gate.
  Failed import does not disconnect or replace a valid unlocked model silently.

Record each result as passed/failed/not-run. This specification is not a security
audit, proof of implementation, or claim of compatibility with prototype files.

## 10. Primary references

- [NIST SP 800-38D: GCM](https://csrc.nist.gov/pubs/sp/800/38/d/final).
- [OpenSSL EVP AEAD interface and GCM final verification](https://docs.openssl.org/3.5/man3/EVP_EncryptInit/).
- [OpenSSL RAND_bytes / RAND_priv_bytes](https://docs.openssl.org/3.5/man3/RAND_bytes/).
- [RFC 9106: Argon2, version/parameters/test vectors](https://www.rfc-editor.org/rfc/rfc9106).
- [RFC 8259: JSON and interoperability](https://www.rfc-editor.org/rfc/rfc8259).
- [RFC 9562: UUID layout](https://www.rfc-editor.org/rfc/rfc9562).
- [Qt QSaveFile and direct-write fallback](https://doc.qt.io/qt-6/qsavefile.html).
- [Qt QLockFile and stale-lock handling](https://doc.qt.io/qt-6/qlockfile.html).
