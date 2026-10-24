# Native stage 2: connections and data

Accepted: 2026-10-24. Implementation commit:
`54fc51ba88f9edf4e728636e88299713d6b3ce8b`.

This closes implementation-plan stage 2's UI-independent module gate, not the
native application release. The current engineering pane still exposes only a
subset of these APIs. No native installer/portable release was published.

## Implemented scope

| Module | Delivered behavior |
| --- | --- |
| ConnectionManager | Async DNS/connect/trust/auth/shell, bounded worker admission, cancellation/deadlines, reconnect, key/password/certificate APIs, bounded multi-round keyboard-interactive/2FA and configurable keepalive |
| Vault document | Strict typed records/references, canonical endpoint-bound credentials, stable host-key identities, groups/hosts/favorites/snippets/appearance; unknown fields and resource violations fail admission |
| VaultService | Independent encrypted vaults, unlock/lock/switch, activity leases, snapshot stale-write detection, cooperating process lock, private ACL/modes, atomic saves, encrypted predecessor, fresh encrypted export and explicit full-replacement import |
| Device keys | Optional current-user Windows DPAPI / Linux Secret Service DEK storage, identity binding, forget, password fallback; no plaintext key fallback or prototype migration |
| TransferManager | Dedicated approved/authenticated SSH transport, bounded jobs/progress, SFTP listing/CRUD, file/tree upload/download, explicit overwrite, private/exclusive staging, cancellation and complete-job deadlines |
| Network routes | Loopback-only local listener, bounded concurrent TCP streams, half-close, one-hop jump composition with separate trust/auth for both hosts and no local destination-DNS lookup |

Key/signature distinction follows RFC 8332: RSA-SHA2 retains the `ssh-rsa`
public-key blob label. Recording that stable identity does not enable SHA-1.
Session's explicit negotiation allowlists exclude SHA-1 signature algorithms.

## Executed evidence

- Windows Release combined CTest at the accepted implementation: **28/28 in
  162.91 seconds**, with actual DPAPI, 22 manager modes, eight SFTP modes, four
  forwarding/jump modes and native connection-pane/terminal regression.
- [Linux full-stack run 40108221514](https://github.com/NetGraniz/NordSSH/actions/runs/40108221514):
  **29/29 ASan/UBSan tests in 108.44 seconds**. Includes actual isolated Secret
  Service, ordinary and negative auth/transfer cases, a real 128 KiB full tmpfs
  (10.44 seconds), permission-denied atomic rename and real private OpenSSH
  public-key/SFTP/direct-forwarding (1.10 seconds). Its RSA host permits only
  RSA-SHA2. The same workflow passed standalone Qt/VT/DNS **13/13**, real TUI
  xcb/Wayland scenarios and isolated X11 physical input/clipboard.
- [Ed25519 OpenSSH run 40107531945](https://github.com/NetGraniz/NordSSH/actions/runs/40107531945):
  **29/29 in 83.43 seconds** after the direct-channel EOF correction.
- [Core run 40108220623](https://github.com/NetGraniz/NordSSH/actions/runs/40108220623):
  Windows/Linux structural, structural sanitizer and pinned crypto/SSH sanitizer
  jobs all green. No sanitizer suppression was added.

Fixtures use fresh temporary roots, synthetic credentials and in-memory client
keys. Real sshd runs only on an explicitly enabled disposable GitHub runner with
its own loopback config, host keys and unique temporary account. It does not
modify the system sshd config. The full-disk fixture fills only its 128 KiB tmpfs.
The Linux device fixture uses a private D-Bus session and disposable HOME/XDG
keyring. Production hosts, Minecraft files, user vaults/backups and signing keys
were not used.

## Boundaries retained for the next stages

Directory transfers are atomic per file, not across an entire tree. Cancellation
discards the affected SFTP transport; pending work needs a new authenticated
connection. Unsupported safe-commit capabilities fail rather than unlink an old
target. Remote staging can remain after transport loss. A lost commit reply is
`CommitUnknown`: refresh before retrying, since either complete version may exist.
Snapshot/mtime checks are not protection against malicious same-user/server races.

Stage 3 must wire these modules into the full Qt Widgets host/vault/auth/SFTP/
snippet/settings screens and the shared activity registry. Stage 4 must deliver
and verify complete runtime bundles, installers and signed rollback-capable
updates. Stage 5 still requires full N01–N22 end-user/platform acceptance.
Component passes do not establish those UI, clean-machine or update outcomes.
