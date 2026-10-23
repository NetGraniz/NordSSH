# NordSSH

Local SSH and SFTP client for Windows and Linux. No cloud account, vault sync
service or subscription. Version `0.1.0` adds installers and signed updates to
the earlier portable beta.

## Connections and terminal

- Passwords, Ed25519/ECDSA private keys and compatible OpenSSH user certificates.
  RSA is disabled in this beta pending a safe backend; see SECURITY.md.
- Terminal tabs, resizing, right-click paste, multiline/control-character paste
  confirmation, two themes and adjustable font size.
- Hosts organized by vault labels, favorites and search; saved command snippets.
- Unknown server keys require confirmation before authentication. Changed trusted
  keys are refused. Host-certificate CA trust is not implemented.
- Settings lists trusted server fingerprints. Removing one requires native
  confirmation with all sessions closed; the next connection asks for trust again.
- SFTP browsing, downloads finalized through a temporary local file, and uploads
  using exclusive creation. Existing remote files are not overwritten. A failed
  upload may leave a partial remote file.

## Local, portable vault

The same password-encrypted `.nordvault` file works on Windows and Linux. It holds
hosts, saved passwords, private keys, user certificates, trusted fingerprints,
snippets and appearance settings. Encryption uses RustCrypto AES-256-GCM and
Argon2id (64 MiB, three passes, parallelism one). A random vault key encrypts data;
the password-derived key wraps that key. Format parameters are fixed and versioned,
not accepted as unbounded work requests from imported files.

Choose a password of at least 7 characters at first use; a long phrase is safer. Keep it separately:
NordSSH cannot recover a forgotten password. **Remember on this device** avoids
re-entering it on that computer. Only the unlock key is protected by Windows DPAPI
or Linux Secret Service. A new computer needs the vault password once; device
unlock records are not bundled in exports. If Secret Service is unavailable,
password unlock still works. There is no plaintext fallback.

Settings provides encrypted export/import, locking and forgetting the remembered
device key. Export bundles referenced key files; a missing referenced key causes
an error rather than an incomplete backup. Import replaces the whole current vault
after confirmation, retains an exact encrypted before-import backup, and does not
merge. Only import trusted files: their server trust records are imported too.
Active sessions must close before locking or importing.

An earlier Windows `vault.dpapi` is migrated after setting a portable-vault
password. Its original file stays untouched. Saved secrets never appear in
frontend snapshots. This is a beta, not an audited password manager; keep backups.

## Installers, portable mode and updates

Download the Windows `-setup.exe` installer or the Linux `.deb` package from
[Releases](https://github.com/NetGraniz/NordSSH/releases). The Windows installer
installs for the current user, adds Start-menu shortcuts and includes the WebView2
bootstrapper. Linux packages declare GTK/WebKit dependencies. Installing a newer
package over the old one preserves the user-profile vault and device unlock keys.
Portable archives remain available separately.

Extract the full portable archive into a writable folder. Keep `nordssh.portable`
beside `NordSSH.exe` or `NordSSH.AppImage`. Data then lives in
`NordSSH-data/vault.nordvault` beside the application, independently of the working
directory. `--portable` enables the same mode. Without it, data lives in the
platform app-data directory for `com.nordssh.desktop`.

To update, close the program and replace the executable/AppImage, README and
marker. **Keep `NordSSH-data` intact.** Remembered unlock material stays in the
OS profile, outside the portable folder. Linux AppImage needs a compatible desktop
and may need FUSE or extract-and-run mode. Portable does not mean every operating
system library is bundled.

The header checks stable public GitHub Releases; it does not announce prereleases.
When an update is available, click **Update** and confirm the native prompt.
Close SSH sessions and transfers first. NordSSH downloads a bounded package,
verifies its minisign signature and signed version, creates an encrypted vault
backup, then installs and restarts. A damaged payload, version mismatch or
untrusted download source is refused before installation.

Windows installed copies use the installer. Portable Windows copies use a
separate signed EXE replacement helper, retain the portable marker/data folder,
and keep the previous executable for recovery. Linux uses its matching Debian
or AppImage package; Debian installation can request OS administrator permission.
No GitHub token is embedded in the client. Update checks can be disabled.
Update signatures are not Windows Authenticode certificates: this first installer
can still show an unknown-publisher/SmartScreen warning.

## Development

Stack: Tauri 2, Rust, TypeScript, xterm.js, `russh`, `russh-sftp`. `ssh2` is a
development-only loopback fixture, not a shipped Node.js runtime. Node.js 24 and
Rust 1.99.1 are pinned by the build. Windows needs C++ Build Tools, a Windows SDK
and WebView2; Linux needs Tauri's GTK/WebKit development packages.

```text
npm ci
npm run build
npm test
cargo test --locked --manifest-path src-tauri/Cargo.toml --lib -- --test-threads=1
npm run tauri -- dev
```

Release builds run in GitHub Actions on Windows and Ubuntu 22.04. Loopback tests
use generated synthetic keys and credentials, never production hosts. See
TESTING.md for the scope and SECURITY.md for limitations.

Not included in 0.1: split panes, SSH Agent, keyboard-interactive/2FA prompts,
tunnels, jump hosts and transfer progress/resume/cancellation. Account sync and
telemetry are not part of NordSSH.
