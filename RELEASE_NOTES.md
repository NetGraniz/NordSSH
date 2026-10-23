# NordSSH 0.1.0-beta.1

First portable beta for Windows x64 and Linux x64. SSH, SFTP and a local encrypted
vault; no cloud account, subscription or telemetry. This is an early release,
not a stable client or an audited password manager. Keep independent backups.

## Download and start

- Windows: extract `NordSSH-0.1.0-beta.1-windows-x64-portable.zip` into a writable
  folder and run `NordSSH.exe`. Microsoft WebView2 Runtime is required.
- Linux: extract `NordSSH-0.1.0-beta.1-linux-x64-portable.tar.gz` and launch
  `NordSSH.AppImage`. Built on Ubuntu 22.04 x64; a compatible GTK/WebKit desktop
  and FUSE or AppImage extract-and-run support are required.
- Keep `nordssh.portable` beside the application. Your data lives in
  `NordSSH-data/vault.nordvault`, not in the release archive or GitHub.

Choose a vault password of at least 12 characters. **Remember on this device**
can save repeat unlocks through Windows DPAPI or Linux Secret Service. Moving to
another computer requires the vault password; there is no password recovery.
If Secret Service is unavailable, password entry still works without saving a
plaintext key.

## Included

- SSH terminal tabs, password and Ed25519/ECDSA key authentication, key-file
  passphrases and compatible OpenSSH user certificates.
- Host search, favorites, vault labels, saved commands, two themes and font size.
- Right-click paste and confirmation for multiline/control-character pastes.
- Confirmation of unknown server fingerprints; changed trusted keys are refused.
- SFTP browsing, downloads and uploads without overwriting existing remote files.
- Password-encrypted export/import of hosts, passwords, embedded keys, trusted
  fingerprints, snippets and settings. Import replaces the active vault after
  confirmation and saves an encrypted backup; it does not merge.
- Windows/Linux-compatible vault encryption: AES-256-GCM and Argon2id, with
  authenticated metadata and bounded file sizes.
- Migration of the earlier Windows DPAPI vault without deleting its original.
  A missing referenced key aborts migration instead of creating a partial vault.

## Limits to know

**RSA keys are disabled** in this beta because of RUSTSEC-2023-0071 in the RSA
dependency. Use Ed25519/ECDSA keys and compatible server host keys; password
authentication is still available. A documented upstream GLib iterator fix is
backported for the Linux GTK dependency. See `SECURITY.md` in the repository.

Automatic update installation is not enabled. The header checks **stable** GitHub
releases, not prereleases like this beta. To update manually, close NordSSH and
all sessions/transfers, replace the executable/AppImage and bundled README/marker,
and **keep `NordSSH-data` intact**.

SSH Agent, keyboard-interactive/2FA, jump hosts, tunnels, transfer progress/resume,
and editable host-key trust are not included. Host-certificate CA trust is not
implemented. User-certificate authentication is separate and needs broader
interoperability testing. A failed upload can leave a partial remote file.

Both platforms run isolated SSH/SFTP and vault tests with synthetic credentials.
Browser tests mock IPC; native startup smoke checks are not full GUI testing.
No production server or real SSH credential was used. `TESTING.md` records build
provenance, checks and remaining work. Bundled `licenses/` contains third-party
notices. `SHA256SUMS.txt` checks download integrity; it is not a code signature.
The Windows executable is not Authenticode-signed and may trigger publisher or
SmartScreen warnings. No installer is included in this beta.
