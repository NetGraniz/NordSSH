# NordSSH development

## Native rewrite decision, 2026-10-23

- Read docs/NATIVE_REQUIREMENTS.md and docs/adr/0001-native-client.md before coding.
- The owner rejected the web runtime and authorised a fresh native implementation.
  Use the agreed C++20/Qt Widgets/libssh/OpenSSL/Argon2 direction. No WebView,
  HTML/JS UI, Rust backend/FFI/stdio bridge, or transfer of the Tauri application.
- Preserve existing user vaults/backups/signing keys. Native data uses a separate
  namespace; do not auto-read, migrate, replace or remove prototype data.
- Validate the native terminal and crypto/SSH gates before migrating the visual
  design. Never substitute a text box/ANSI regex or a mocked shell for a terminal.
- Installer/update code under src-tauri is prototype-only. Do not publish it as a
  native client; its EXE-only portable update is not suitable for Qt deployments.
- Qt patch, exact dependencies and Windows/Linux packaging must be verified and
  pinned before their binary gate. A native release requires full-bundle update,
  rollback, real native UI tests, and no browser-runtime dependency.

## Repository and safety

- Verify the repository origin, branch and worktree before editing. Pull with
  `git pull --ff-only` on an established branch before each new task. Preserve
  unrelated changes.
- Keep credentials, connection exports, signing private keys and real user data
  out of Git. Ignore rules alone are not a security boundary; review staged files.
- Do not connect to or change production hosts to test the client without explicit
  permission. Use isolated fixtures and synthetic credentials.
- Keep UI status honest: planned features and mocks are not working SSH sessions.
- Never bypass host-key validation, accept changed keys silently, log secrets,
  or pass passwords in process arguments.
- Keep the vault format independent of the OS. Use password-protected portable
  encryption, with optional Windows DPAPI/Linux Secret Service ONLY for remembering
  the device unlock key. Do not save a plaintext unlock key beside a portable vault.
  Retain the old DPAPI file unchanged during migration.
- Updates must verify signatures, preserve data and require confirmation before
  interrupting live sessions or transfers. Do not publish an installer until its
  installation and upgrade path have been tested.
- Check current primary documentation for protocol, security and packaging choices.
