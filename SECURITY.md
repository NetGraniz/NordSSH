# Security and development status

NordSSH is a development build, not an audited credential manager. Keep an
independent backup of keys and access details. Do not publish secrets in issues,
screenshots, terminal transcripts or GitHub repositories.

## Local protection

The vault is encrypted with Windows DPAPI for the current user, without
`CRYPTPROTECT_LOCAL_MACHINE`. No application master password is required. DPAPI
does not defend against malware running as the same user or a compromised Windows
administrator. Decrypted credentials must exist in process memory during use;
zeroization reduces exposure but cannot erase all copies held by the OS, IPC,
JavaScript engine or SSH library.

Vault writes use a same-directory temporary encrypted file and atomic replacement.
Invalid files and unsupported schemas cause an error, not an empty vault reset.
There is no migration to a newer schema yet. Real credentials never belong in tests.

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
