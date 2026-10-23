# Development checks

## 0.1.0-beta.1 — portable vault, 2026-10-23

Local checks passed portable encryption/decryption, wrong-password rejection,
authenticated header/wrapped-key tampering, encrypted export/import preserving
credentials/settings/trusted keys, locked-write refusal, corruption and stale-file
preservation, Windows device unlock, and legacy DPAPI migration preserving the
original bytes. The generated Windows interchange fixture uses only synthetic
data and the public password `synthetic-only-vault-password`.

The browser IPC-mock suite additionally checks first-vault setup, password mismatch,
wrong-password retry, non-dismissable lock and unlock. This remains a mocked UI
test, not proof of all native dialogs or clipboard behavior. Linux and Windows
release checks and reverse interchange verification are pending at this record's
initial commit; no successful result is assumed before the jobs complete.

Dependency review found the GTK GLib iterator issue and RSA timing advisory.
The former carries the documented upstream two-line backport; RSA is disabled.
Remaining informational maintenance advisories are listed in SECURITY.md.

The first release-build run was cancelled after these review findings. Its
artifacts are not the release. A follow-up builds the corrected source and adds
a D-Bus session to the isolated Linux window startup smoke check.

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

## Before a distributable release

- Test the actual native dialogs, clipboard behavior, terminal flow control,
  close/reconnect races, large transfers and error handling through the Windows UI.
- Test user certificates, supported key formats and real OpenSSH interoperability;
  only password and encrypted Ed25519 key auth were exercised end to end here.
- Add and test portable encrypted export/import, including key-file handling,
  conflicts, malicious inputs, partial failures and backup recovery.
- Configure signed updater artifacts; test rejected signatures, offline GitHub,
  active sessions, installer upgrades and preserved user data.
- Review Rust security advisories, dependencies and credential handling. Passing
  these tests is not a security audit. No production-readiness claim is made.
