# Development checks

## 0.1.0-beta.1 — portable vault, 2026-10-23

Local checks passed portable encryption/decryption, wrong-password rejection,
authenticated header/wrapped-key tampering, encrypted export/import preserving
credentials/settings/trusted keys, locked-write refusal, corruption and stale-file
preservation, Windows device unlock, and legacy DPAPI migration preserving the
original bytes. The generated Windows interchange fixture uses only synthetic
data and the public password `synthetic-only-vault-password`.

The browser IPC-mock suite uses the compiled production frontend and additionally
checks first-vault setup, password mismatch, wrong-password retry, non-dismissable
lock and unlock. All four Node tests and the browser suite passed without browser
console errors. This remains a mocked UI test, not proof of all native dialogs or
clipboard behavior.

Windows release job 120383349440 passed all 17 native tests, Clippy with warnings
denied, formatting, frontend build and the production npm dependency audit. The
downloaded portable EXE also created a responding native window; that test-owned
process was stopped without entering credentials or connecting to a host.
Its SHA-256 is `5ac50038dfc096ad43708281563fe6edc4d87ce656051840e3d3e670cc47ae5b`.

Additional local assertions passed within the same 17-test suite: SSH authentication
with an embedded encrypted key after deleting its synthetic source file, legacy
DPAPI migration embedding referenced key material, and missing-key migration
refusing to create a partial portable vault while preserving the original bytes.

Dependency review found the GTK GLib iterator issue and RSA timing advisory.
The former carries the documented upstream two-line backport; RSA is disabled.
Remaining informational maintenance advisories are listed in SECURITY.md.

The first release-build run was cancelled after these review findings. Its
artifacts are not the release. The next Linux job passed its native tests but
failed AppImage packaging because no square icon had been selected. A Linux-only
retry selects the existing 128x128 icon explicitly.

Linux retry job 120388892950 passed all 17 native tests, Clippy with warnings
denied, formatting, Node tests, frontend build and production npm audit, then
successfully built an AppImage. The AppImage created a native `NordSSH` window
under Xvfb with an isolated D-Bus session. This is startup smoke coverage, not
verification of every desktop environment or real Secret Service configuration.

Build provenance: Windows comes from `65f4ee89350cbb455ba757efd9aa3860e27520ed`
(run 39944390662, successful Windows job; its earlier Linux packaging job failed).
Linux comes from `64a9fc82bff2112edff1c230f8fbd6f77a60030b`
(successful Linux-only run 39946058558). The latter changes packaging and test
helpers only; `src/`, `src-tauri/` and `vendor/` are identical between those two
build commits. Both use version `0.1.0-beta.1`. Third-party notices were added
after downloading the build archives without changing the binaries.

Windows-to-Linux interchange passed in both Linux build jobs using the checked-in
synthetic Windows fixture. Reverse interchange also passed on the local Windows
machine using `interchange-Linux.nordvault` downloaded from run 39946058558:
embedded key material, trusted fingerprint and font size were preserved.
The independently downloaded Windows runner fixture also unlocked on local
Windows. The synthetic fixtures and their public password are test data, never
templates for a real vault.

Final archive checks found only the application, portable marker, README and
third-party notices; no generated vault, device key or test identity was included.
Repacking preserved the Windows EXE and Linux AppImage hashes, and the Linux
AppImage retained executable mode. The AppImage SHA-256 is
`80c90484f7f3ea674a0db226800f6486cd0ec19fc7e028af001b44d242931f0c`.
Windows Authenticode inspection reported `NotSigned`; this preview does not claim
signed distribution or a tested installer. Release archive hashes are published
in `SHA256SUMS.txt` alongside the downloads.

## 0.1.0-dev.1 — Windows, 2026-10-23

Environment: Rust 1.99.1, Node.js 24.19.0, Visual Studio Build Tools 2022 with
MSVC and Windows SDK, WebView2 Runtime already installed. No production server,
user SSH key or saved production credential was used.

The Rust suite passed 14 tests. Its loopback-only SSH fixture generates fresh
Ed25519 keys and uses synthetic credentials. Coverage includes:

- Password authentication, PTY/shell requests and terminal input/output.
- Encrypted private-key authentication and rejection of a wrong passphrase.
- Wrong-password rejection and refusal of a changed trusted host key.
- SFTP listing, download, upload/read-back and refusal to overwrite an existing
  remote file through exclusive create.
- DPAPI encryption/decryption and tamper rejection, reopening without a master
  password, no plaintext password in the saved vault, and failed-edit rollback.
- Corrupt file preservation, unsupported schema rejection and validation limits.
- Login identity changes invalidating retained credentials.

Three Node tests cover multiline/control-character paste detection, safe remote
filename joining and bounded font size. The headless browser test checks empty
state, host editing and authentication fields, host search/rendering, theme,
snippets and update UI. Its IPC is mocked: it does not establish that the native
webview successfully completed an SSH connection.

A debug Windows EXE was built using Tauri with embedded frontend assets. A startup
smoke run created a responding native window titled `NordSSH`, then the test-owned
process was stopped. This is not a full native GUI end-to-end test. The browser
screenshots are local ignored test artifacts, not production screenshots.

Retained development failures: the initial SSH test assumed the first channel
message was terminal data; the server legitimately sent request success first.
The fixture now waits for a bounded expected data marker. The first browser test
timed out while fetching the intercepted entry document; it now serves the local
entry directly and uses an isolated direct-connection browser. A first TLS-backend
build failed in the transitive AWS-LC C build; the application uses Rustls/ring
instead, with certificate verification left enabled. No validation was disabled
to make the tests pass.

## Remaining before a stable release

- Test the actual native dialogs, clipboard behavior, terminal flow control,
  close/reconnect races, large transfers and error handling through the Windows UI.
- Test user certificates, supported key formats and real OpenSSH interoperability;
  only password and encrypted Ed25519 key auth were exercised end to end here.
- Extend portable encrypted export/import testing to full native UI flows,
  concurrent external file changes and manual backup recovery. The automated
  encryption, invalid-input and partial-migration tests already pass.
- Configure signed updater artifacts; test rejected signatures, offline GitHub,
  active sessions, installer upgrades and preserved user data.
- Review Rust security advisories, dependencies and credential handling. Passing
  these tests is not a security audit. No production-readiness claim is made.
