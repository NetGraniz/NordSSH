# Native implementation evidence

Date: 2026-10-23. This is an engineering checkpoint, not a release announcement.
The new client does not yet support real SSH/SFTP connections or on-disk vaults.
The user is not asked to test intermediate harnesses or mock interfaces.

## Completed locally

- Verified canonical origin/branch and successful fast-forward pull before work.
- Installed Qt 6.12.0 base/tools/translations SDK; `qmake`, `qtpaths6` and
  `windeployqt` run and report 6.12.0. HTTPS independent mirrors, published SHA1 and
  matching SHA256 pins were checked. These pins are first-acquisition evidence,
  not upstream signed SHA256 attestations. See NATIVE_TOOLCHAIN.md.
- Built native C++20 core with MSVC 19.44.35229 and Windows SDK 10.0.26100.0.
- `vault_format`: passed, fixed header/length/profile/revision admission, strict
  UTF-8 scalar count and seven-symbol minimum, bounded password input, exact AAD.
  This test does not exercise AES-GCM, Argon2, file transactions or JSON parsing.
- `paste_router`: passed with Qt 6.12.0 offscreen, synthetic key/mouse events for
  Ctrl+V, Ctrl+Shift+V, Shift+Insert, Windows RU virtual-key path, key-release/
  autorepeat consumption, Ctrl+C/AltGr/IME separation, parent QShortcut conflict,
  reentrant callback suppression and one right-click action.
  No real clipboard, native keyboard layout, byte-over-SSH or Wayland test yet.
- Read-only security review approved structural admission within its limited
  scope. Added independent exact domain-byte fixtures and boundary regressions.

Latest local command after configuring the Qt input gate:

```text
cmake --build native/build-vault --config Debug --parallel 4
ctest --test-dir native/build-vault -C Debug --output-on-failure
2/2 passed: paste_router (0.23 s), vault_format (0.07 s)
```

Source was an uncommitted working tree at that run; see the enclosing Git commit
for these sources. Raw local logs remain under the ignored build directory.
GitHub native core/sanitizer jobs rerun the committed source independently.
[Run 39957664492](https://github.com/NetGraniz/NordSSH/actions/runs/39957664492)
passed all three jobs for commit `99f0ec689b057679a9711821ad5c1ead135026aa`:
Windows core, Ubuntu 24.04 core and Ubuntu ASan/UBSan. They test vault structural
admission, not Qt input, encryption, real SSH or complete application acceptance.

## Terminal decision

The official libvterm 0.3.3 source compiled with MSVC; a limited happy-path test
passed (0.22 s). Source review then found security-blocking parser/index/numeric
and combining-state paths. The engineering lead rejected production integration.
The experiment is isolated, OFF by default and not linked to product modules.
See NATIVE_TEST_MATRIX.md for scope and nonclaims.

libghostty-vt is the next candidate, not an approved release dependency. Its
Windows feasibility and bounded-memory behavior need their own recorded results.
The official Zig 0.17.0 download was hash-verified; initial test attempts hit Zig
TLS/cache acquisition problems, not failed executed VT assertions. A retry is
being evaluated; neither its runtime test gate nor ReleaseSafe build is yet
recorded as passed. Do not infer success from a running compiler.

## Second checkpoint: authenticated encryption

Implemented `native/src/vault/VaultCrypto.*` and tests, using OpenSSL 3.5.9
and hash-pinned Argon2 20190702. The OpenSSL source archive was checked against
upstream SHA256/GitHub digest and a mathematically verified detached signature;
the signature check is not a full OpenPGP revocation/trust assessment. Acquisition
records are in local `.tools/nordssh-deps/{acquisition,verification}.json`.

Windows MSVC Debug CTest passed `vault_crypto` (6.96 s) and `vault_format`
(0.03 s). Tests used the freshly built OpenSSL source-tree DLL/import library;
the complete OpenSSL installation and packaging gate were not yet complete.

Executed assertions cover:

- Official AES-256-GCM known-answer vectors with and without AAD, including tag
  failure and empty low-level GCM input. Vault payloads cannot be empty.
- RFC 9106 section 5.3 Argon2id vector and the application's fixed 64 MiB/t3/p1
  profile against separately packaged argon2-cffi output.
- Opening a frozen envelope generated independently by argon2-cffi 25.1.0 /
  bindings 26.1.0 and cryptography 50.0.2. The deterministic generator is checked
  in under `native/tests/reference/`; its fixed secrets/nonces are synthetic only.
- Password/key opening, wrong password/key, mutation of every file byte including
  the wrapping tuple during device-key opening, truncation, trailing bytes,
  cross-vault wrapper splicing, revision increment, fresh save nonce and unchanged
  password wrapper during ordinary save.
- Move-only secret ownership; cleansing is implemented with OpenSSL clear/free.
  This is not a claim of protection against crash dumps or same-user malware.

These are in-memory cryptographic envelopes, not a complete vault service. Strict
JSON/schema validation, disk transactions/ACLs/locking, cancellation, RNG/provider
fault injection and device storage still require their own tests. Unauthenticated
plaintext is kept private and cleansed on failure; no JSON is parsed by this layer.

## Next exit criteria

1. A safe pinned VT engine plus Qt raster renderer, real loopback SSH terminal,
   resize/fullscreen programs and byte-level single-paste verification.
2. AES-GCM/Argon2 and persistence tests against the new vault format, then saved
   hosts/auth/trust/SFTP/jump/local-forward integration with synthetic fixtures.
3. Finished native interface and full Windows/Linux installers/portable updates.

Existing prototype vaults, backups, signing keys and production hosts were not
read, changed or used as test data. No native release has been published.
