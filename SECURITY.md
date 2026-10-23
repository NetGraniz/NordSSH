# Security and development status

NordSSH is a development build, not an audited credential manager. Keep an
independent backup of keys and access details. Do not publish secrets in issues,
screenshots, terminal transcripts or GitHub repositories.

## Local protection

The portable vault uses AES-256-GCM with a random data key. Argon2id derives a
wrapping key from its password (64 MiB, three passes, parallelism one). The header
and wrapped-key metadata are authenticated. OS protection is optional and only
remembers a device unlock key: user-scoped Windows DPAPI or Linux Secret Service.
Device unlock records are never bundled in portable exports. An unavailable or
locked system store falls back to password entry, never plaintext storage.
This does not defend against malware running as the same user or a compromised
administrator. Decrypted credentials must exist in process memory during use;
zeroization reduces exposure but cannot erase all copies held by the OS, IPC,
JavaScript engine or SSH library.

Vault writes use a same-directory temporary encrypted file and atomic replacement.
Invalid files and unsupported schemas cause an error, not an empty vault reset.
Legacy Windows DPAPI migration retains the original file unchanged and embeds
referenced SSH key files before committing. A missing key aborts migration/export.
Encrypted imports retain exact-byte backups of the previous active vault.
Real credentials never belong in tests; the published interchange fixture contains
only synthetic data and has a public test password.

## Dependency review for beta 1

An OSV/RustSec scan of the lockfile identified the GLib iterator unsoundness
RUSTSEC-2024-0429. The GTK3 dependency cannot simply use GLib 0.20 types, so
`vendor/glib-0.18.5` carries the exact two-line upstream fix with its original MIT
license. See vendor/PATCHES.md. Linux tests exercise the affected iterator.

RSA support is disabled in this beta rather than shipping the RSA dependency
flagged by RUSTSEC-2023-0071. Use Ed25519 or ECDSA keys and compatible server host
keys. Password authentication remains available. This is a compatibility limit,
not a claim that all RSA implementations are unsafe.

Informational unmaintained-package advisories remain for transitive GTK/D-Bus
dependencies (`proc-macro-error`, `zvariant`, `zvariant_derive`, `zbus_names`).
They are not evidence of an exploited vulnerability, but still require upstream
maintenance tracking. A dependency scan is not a penetration test or a full audit.

## SSH identity

Unknown host keys require native confirmation before user authentication. Verify
the SHA-256 fingerprint independently. An existing different key is refused;
there is no automatic key replacement. User password changes do not authorize a
host-key change. Host certificates are currently refused until a CA trust policy
is implemented. User certificates are a separate authentication feature.

## Terminal and files

Remote terminal bytes go to xterm.js, not HTML. The renderer cannot call a generic
local shell or generic local filesystem API. Native file dialogs choose download
destinations, upload sources and key files. Clipboard pastes containing control
characters/newlines require confirmation. Bracketed paste support depends on the
remote shell. Do not paste untrusted commands merely because a prompt appeared.

SFTP uploads use exclusive create and do not overwrite an existing remote file.
Failed uploads may leave a partial file. Downloads replace the chosen local target
only after transfer and flush succeed. There are not yet transfer cancellation,
resume controls, or remote destructive operations.

## Distribution

This build checks public GitHub release metadata without a GitHub token. It does
not download or execute updates. Automatic installation must remain disabled until
signed package verification, data preservation and the installer upgrade path pass
tests. A release on GitHub alone is not sufficient proof of package authenticity.
