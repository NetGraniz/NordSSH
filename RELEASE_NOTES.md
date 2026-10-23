# NordSSH 0.1.0

SSH/SFTP client for Windows and Linux with a local encrypted vault, saved commands,
terminal tabs and themes. No cloud account or telemetry.

## What to download

- **Windows installer:** `NordSSH-0.1.0-windows-x64-setup.exe`. Installs for the
  current user and adds Start-menu shortcuts. The WebView2 bootstrapper is included;
  downloading the runtime requires internet if it is not already installed.
- **Linux installer:** `NordSSH-0.1.0-linux-amd64.deb` for compatible Debian/Ubuntu
  desktops. The package declares its GTK/WebKit dependencies.
- **Portable:** the Windows `-portable.zip` or Linux `-portable.tar.gz` archive.
  Extract everything to a writable folder and keep `nordssh.portable` beside the
  program. Data stays in `NordSSH-data` there.
- The raw `.AppImage` is also available. The Windows EXE without `-setup` and the
  `.sig` files are signed-update assets, not installers.

## First launch

Create a new vault password of **at least 7 characters** and repeat it. This is
not your Windows or SSH-server password. A longer phrase protects backups better.
The form now distinguishes a short password from a mismatched repeat and can show
the entered password. **Remember on this device** avoids repeated unlock prompts.
Keep the vault password separately; NordSSH cannot recover it.

## Installing updates

Installing a newer setup/package over the existing one keeps the profile vault
and remembered keys. Portable users must retain `NordSSH-data` when replacing files.
The header also checks stable GitHub releases and offers an **Update** button.
After native confirmation, the program downloads the matching package, verifies
its minisign signature and signed version, saves an encrypted vault backup and
installs it. Close SSH sessions and transfers first. Linux package installation
can request OS administrator permission. Portable Windows updates replace only
their EXE and retain the previous executable for recovery.

Saved fingerprints are now visible in Settings. Removing trust requires explicit
confirmation; a changed server key is never accepted automatically. Changing an
SSH password does not require removing its server fingerprint.

## Compatibility and checks

RSA remains disabled because of a dependency advisory; use Ed25519/ECDSA keys and
compatible server host keys, or password authentication. User certificates are
supported separately from host-certificate CA trust, which is not implemented.
The Windows installer is not Authenticode-signed and may show publisher/SmartScreen
warnings. Updater minisign signatures do not replace that OS certificate.

See `TESTING.md` for exact native, protocol, installer and retention checks, and
`SECURITY.md` for limitations. This is not an audited password manager. Keep
independent backups. `SHA256SUMS.txt` checks download integrity, not publisher
identity. Third-party notices are included in the packages.
