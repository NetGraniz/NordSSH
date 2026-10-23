# NordSSH

A Windows SSH and SFTP client with local connection vaults, customizable terminals
and updates delivered through GitHub Releases.

## Status

Development build `0.1.0-dev.1`, not a stable release. The Windows frontend and
Rust backend compile. No installer or signed automatic update has been published.
Do not use this build as the only copy of important connection credentials.

Implemented in the development source:

- Password, private-key and OpenSSH user-certificate authentication with `russh`.
- Interactive xterm.js terminal tabs, resizing, bounded output backpressure and
  close confirmation for active connections/transfers.
- Native confirmation of unknown plain host keys; changed keys are refused.
- Windows user-scoped DPAPI vault, atomic replacement and schema validation.
  Saved secrets are not returned in frontend snapshots.
- Host folders (vault labels), favorites, search, saved snippets and two themes.
- SFTP directory browsing, download through a temporary local file, and exclusive
  upload creation. Existing remote files are not overwritten. A failed upload may
  leave a partial remote file; resumable transfers are not implemented yet.
- GitHub latest-stable-release checks in the window header. Installation remains
  disabled until signed packages and the upgrade path are tested.

Not implemented yet: portable export/import, terminal split views, SSH Agent,
keyboard-interactive/2FA prompts, jump hosts, tunnels, transfer progress/cancellation,
trusted host-certificate CAs, and signed update installation. The certificate option
is for a user certificate plus its private key, not an X.509/TLS certificate.

## Build on Windows

Prerequisites: Node.js 24, Rust 1.99.1 (MSVC), Visual Studio C++ Build Tools with a
Windows SDK, and Microsoft Edge WebView2 Runtime. No production server is needed.

```powershell
npm ci
npm run build
npm test
cargo test --manifest-path src-tauri/Cargo.toml --lib -- --test-threads=1
npm run tauri -- dev
```

The SSH integration test requires Node.js and uses a generated, loopback-only
`ssh2` fixture. Its credentials and host key are synthetic. The browser UI test
(`node test/ui.cjs`) uses mocked IPC and an isolated local Chrome profile; it is
not evidence of a working native SSH session.

User data belongs in Tauri's Windows app data directory for `com.nordssh.desktop`,
not next to the executable. Keep the original Windows profile and backups: copying
a DPAPI vault alone to another account or computer is not a portable export.

## Planned features

- SSH password, private-key and OpenSSH certificate authentication.
- Saved hosts organized by folders, tags and favorites.
- Terminal tabs, split views, themes, fonts and keyboard shortcuts.
- Right-click paste with confirmation for multiline commands.
- Reusable command snippets; reconnecting must not replay them automatically.
- SFTP browsing, uploads and downloads with transfer progress.
- Local vaults without an application master password. Windows user-scoped DPAPI
  protects saved secrets; this does not protect against malware running as that user.
- Export without secrets, or password-encrypted portable export with secrets.
- Explicit trust for server host keys, retained across restarts and export/import.
  Changed host keys stop the connection until the user reviews the change.
- SSH tunnels, jump hosts, keepalive and connection diagnostics.

## Updates

The window header will show **Check for updates**, **Update available**, download
progress and **Restart and install**. Stable releases will be discovered through
this repository's GitHub Releases. Automatic checks can be disabled; network
errors must not prevent SSH or SFTP use.

Installation is user-initiated by default. Active SSH sessions and SFTP transfers
require explicit confirmation before the application closes. An optional automatic
download setting must not enable unattended session termination.

The planned Tauri updater will verify signed update artifacts using a public key
embedded in the application. Signing private keys never belong in this repository.
The updater must reject invalid signatures and must not fall back to unsigned
installation. GitHub tokens must not be embedded in public application builds.

Installing a newer version over the existing installation must preserve user data.
User data lives outside the application installation directory. Schema migrations
need verified backups, and an unsupported newer schema must not be overwritten.

## Development direction

Stack: Tauri 2, Rust, TypeScript and xterm.js. `russh` handles SSH and
`russh-sftp` handles SFTP. SSH Agent support is pending. `ssh2` is a development-only
test server dependency, not a Node.js runtime shipped in the desktop app.

References: [Tauri updater](https://v2.tauri.app/plugin/updater/),
[Windows DPAPI](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata),
[OpenSSH](https://man.openbsd.org/ssh).
