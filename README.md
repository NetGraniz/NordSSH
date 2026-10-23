# NordSSH

A Windows SSH and SFTP client with local connection vaults, customizable terminals
and updates delivered through GitHub Releases.

## Status

Planning and initial project setup. No runnable client or installer has been
published yet. The features below describe the intended first version, not
completed functionality.

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

The proposed Tauri updater verifies signed update artifacts using a public key
embedded in the application. Signing private keys never belong in this repository.
The updater must reject invalid signatures and must not fall back to unsigned
installation. GitHub tokens must not be embedded in public application builds.

Installing a newer version over the existing installation must preserve user data.
User data lives outside the application installation directory. Schema migrations
need verified backups, and an unsupported newer schema must not be overwritten.

## Development direction

Proposed stack: Tauri 2 with a Rust backend and a web-based terminal interface.
SSH library selection is pending verification of OpenSSH certificate, agent and
Windows compatibility. Use maintained protocol and cryptographic libraries rather
than implementing SSH or cryptography from scratch.

References: [Tauri updater](https://v2.tauri.app/plugin/updater/),
[Windows DPAPI](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata),
[OpenSSH](https://man.openbsd.org/ssh).
