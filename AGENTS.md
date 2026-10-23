# NordSSH development

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
