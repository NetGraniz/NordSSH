# ADR 0004: typed vault records and worker-owned persistence

Status: implemented engineering modules; application UI integration is later.

The authenticated payload must pass both the bounded structural scanner and the
record/reference schema before publication. Temporary decoded JSON values use
cleansed buffers. A saved credential binds an exact canonical host, port and user;
editing a host cannot silently carry its old secret to a different endpoint.

Known-host `algorithm` records the stable public-key format, as libssh reports it.
RSA-SHA2 retains the `ssh-rsa` key blob/label under RFC 8332; this is not permission
to negotiate the deprecated SHA-1 signature scheme. Session's explicit host-key
and user-auth signature allowlists still exclude SHA-1. Pinning a negotiated hash
instead of key format would also cause false changed-key errors across SHA2 choices.

VaultService owns one unlocked document on a non-GUI worker. Independent vaults
have independent random DEKs and passwords. Failed unlock/switch/import leaves the
previous model untouched. Session leases refuse lock/switch/import while in use.
UI controllers must not pass document references across threads.

A new private directory lives directly under an existing non-symlink parent.
Windows ACLs grant only the current SID; POSIX uses 0700 directories/0600 files.
QSaveFile has no direct-write fallback. A cooperating process lock and byte-exact
snapshot comparison reject stale writes. These checks do not claim protection
against a malicious same-user writer racing the final check/rename, administrator
access, crash dumps, or hardware that loses acknowledged writes on power loss.

Each save keeps one encrypted predecessor (`.previous`). Export creates a fresh
password-protected envelope; device records never enter it. Import is an explicit
full replacement, admitted before mutation, preserving the destination password
and DEK. No implicit merge or old-prototype migration is performed.

Optional device unlock stores only the DEK: current-user DPAPI on Windows and
libsecret/Secret Service on Linux. Identity binds path, vault UUID and key revision.
Remembering is opt-in, failed keyring operations have no plaintext fallback, and
password unlock remains independent. Forget deletes that local device record, not
all copies of a previously stolen key. Secret Service calls have cancellation and
a 30-second watchdog on a worker; Windows DPAPI uses UI-forbidden mode.

Tests use new temporary directories and synthetic vaults. Linux device tests run
under a private D-Bus session, temporary HOME/XDG paths and a disposable keyring.
Injected pre-commit failures are not described as a real full-disk/power-loss test.

Primary API references:

- https://doc.qt.io/qt-6/qsavefile.html
- https://doc.qt.io/qt-6/qlockfile.html
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile
- https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata
- https://gnome.pages.gitlab.gnome.org/libsecret/func.password_store_sync.html
- https://gnome.pages.gitlab.gnome.org/libsecret/func.password_lookup_sync.html
