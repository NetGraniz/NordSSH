NordSSH 0.1.0-beta.1 - portable preview

Windows: extract the whole ZIP into a writable folder, then run NordSSH.exe.
Linux: extract the whole archive into a writable folder, mark NordSSH.AppImage
executable if necessary, then launch it. AppImage requires a compatible Linux
desktop; FUSE or AppImage's extract-and-run mode may be needed.

Keep nordssh.portable beside the application. The vault is stored in
NordSSH-data/vault.nordvault beside it, NOT in a cloud service.

Choose a vault password of at least 12 characters on first use. Keep the password
separate from the application and backups. It cannot be recovered by NordSSH.
Remember on this device uses Windows DPAPI or Linux Secret Service only to
remember the unlock key. If Linux Secret Service is unavailable, enter the vault
password at startup; the vault is NEVER saved unencrypted as a fallback.

Use Settings > Export for a password-encrypted backup containing passwords,
private keys, server fingerprints, snippets and appearance settings. Only import
trusted files. Import REPLACES the current vault after confirmation and retains
an encrypted before-import backup. It does not merge vaults.

Updating a portable copy: close NordSSH and all connections/transfers, then
replace only the application executable/AppImage and bundled README/marker.
Keep the existing NordSSH-data folder. Do not remove it or overwrite it with an
empty copy. Automatic update installation is not enabled in this preview.

On Windows, an older vault.dpapi is migrated after you set a portable-vault
password. The original is left unchanged. Old key-file references are embedded
when exporting; missing key files cause an export error, not an incomplete backup.

This is a beta, not an audited password manager. Keep independent backups.
https://github.com/NetGraniz/NordSSH
