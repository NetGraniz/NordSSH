# NordSSH native engineering workspace

This is the new C++20 implementation. There is no runnable SSH application or
native user release here yet. The web prototype lives outside this directory and
is not used by these targets.

Current product modules:

- `nordssh_vault_format`: bounded UTF-8 password and binary-header admission,
  exact AAD construction, plus bounded authenticated JSON structural admission.
  The scanner rejects decoded duplicate keys, invalid Unicode/grammar, unknown
  root fields/types and the format's numeric/container/resource-limit violations
  before a future DOM/model exists. Only the empty schema-1 document has a complete
  record contract; nonempty records/settings are explicitly marked unreviewed.
  This is not a saved-host model or permission to persist arbitrary records.
- `nordssh_vault_crypto`: AES-256-GCM/Argon2id authenticated in-memory envelopes,
  cleansed move-only secrets, fresh-nonce saves and device-key opening. Optional
  `NORDSSH_BUILD_CRYPTO=ON` requires OpenSSL 3.5.9; Argon2 source is hash-pinned.
  JSON schema validation, disk persistence and an unlocked product model are not
  implemented by this layer. Never use authenticated bytes as validated records.
- `nordssh_input`: one paste event route for a future terminal widget. Tests send
  synthetic Qt events; they do not inspect the user's clipboard or prove actual
  keyboard-layout/IME behavior on a desktop session.
- `nordssh_ssh`: worker-only password/OpenSSH-key/PTY transport with explicit bound host-key
  approval, disabled automatic SSH config and bounded reads. Its optional
  `NORDSSH_BUILD_SSH=ON` fixture uses an ephemeral loopback AsyncSSH server, not a
  production host or local system sshd. Async connection orchestration, saved
  trust policy, keyboard-interactive/2FA, SFTP, jump hosts and forwarding are not implemented yet.
  Private keys stay in memory: Ed25519, ECDSA P-256/384/521 and RSA-SHA2. Optional
  matching OpenSSH user certificates are supported by the transport; this is not
  a host-CA policy or a native authentication screen. The initial import scope is
  OpenSSH only, unencrypted or AES-256-CTR/bcrypt with 1..64 rounds and a bounded
  salt/private section. PEM/PKCS8/PPK and other ciphers need reviewed import-cost
  policies. High-cost/unsupported inputs fail, never silently weaken/re-encrypt a key.
- `nordssh_terminal`: optional pinned Ghostty adapter and raster `QWidget` with
  cell-based Unicode/truecolor/SGR rendering, alternate-screen state, resize,
  bounded engine scrollback with wheel/Shift+PageUp/PageDown/Home/End navigation,
  terminal-state-aware key encoding and a single paste route. No text widget,
  HTML, URL launch or remote clipboard hooks. Its SSH test owns a real loopback
  connection in a test-only worker; it does not implement the product connection
  manager. Bounded mouse press/release/drag/hover/wheel encoding follows the
  remote application's tracking protocol. Selection/search, preedit, fullscreen programs and
  desktop clipboard/layout gates remain open.

Build the dependency-free core with CMake and a C++20 compiler:

```sh
cmake -S native -B native/build-core-clean -DCMAKE_BUILD_TYPE=Debug
cmake --build native/build-core-clean --config Debug
ctest --test-dir native/build-core-clean -C Debug --output-on-failure
```

`NORDSSH_BUILD_QT_INPUT=ON` additionally requires the pinned Qt SDK and the
corresponding `CMAKE_PREFIX_PATH`. Test executables need the SDK runtime on their
process search path. This is a developer setup, not the end-user deployment plan.
See [toolchain notes](../docs/NATIVE_TOOLCHAIN.md) and the
[acceptance matrix](../docs/NATIVE_TEST_MATRIX.md).

## Rejected experiment

`experiments/libvterm` is retained only to document a failed feasibility choice.
Its happy-path tests passed, but source review found security-blocking parser and
resource-management paths. It must not consume SSH/server output. The default
build does not fetch, compile or link it. Its opt-in experimental target is not a
product terminal and cannot satisfy a release gate.

The next engine candidate is pinned libghostty-vt with a small C++ adapter. Its
C API is explicitly unstable. Successful upstream CI or a local static-library
build will not by itself close our renderer, keyboard, memory or SSH gates.

The optional `NORDSSH_BUILD_VT_GATE=ON` uses a verified SDK prefix supplied as
`NORDSSH_GHOSTTY_SDK`. It builds only our C++ ABI assertion executable; it is not
the product renderer. The manual Native VT engineering workflow repeats the
pinned upstream/SDK/ABI checks on Windows and Linux without publishing a release.

`NORDSSH_BUILD_TERMINAL=ON` builds the raster adapter and `terminal_widget` test
using the same SDK prefix and pinned Qt. When `NORDSSH_BUILD_SSH=ON` is also set,
`terminal_ssh` checks real SSH output, one UTF-8 paste, exact mouse bytes and resize using the isolated
fixture. Tests inject a synthetic clipboard source and run Qt offscreen; they do
not read/change the user's clipboard, layout, vault or saved connections.

`payload_admission` exercises exact/over-limit bytes, strings, keys, arrays,
objects, depth and node count, malformed UTF-8/escapes, duplicate names and root
shape. With a test Python interpreter available, `payload_oracle` independently
cross-checks 500 generated valid documents and 10,000 seeded mutations using
Python's strict JSON decoder. Python is not a product dependency. The oracle is
a bounded regression corpus, not exhaustive fuzzing or a persistence test.

`ssh_keys` uses ephemeral AsyncSSH keys and user certificates on loopback, with
synthetic key material piped through binary stdin rather than argv or disk files.
It checks encrypted/unencrypted keys, modern RSA signatures, matching certificates,
wrong passphrases, trust-before-auth, KDF-cost bounds, key mismatch and server
rejection of expired/wrong-principal certificates. Its optional `bcrypt==5.0.0`
Python wheel is a test dependency only, not a shipped runtime/backend.

`terminal_history` additionally checks primary-history navigation, alternate-screen
isolation, retaining the viewed history during new output, returning to output on
input, partial wheel deltas, local navigation key releases under Kitty mode and
bounded extreme deltas. It uses synthetic offscreen events. With a remote mouse
mode active, Shift+wheel explicitly requests local history.

`terminal_mouse` checks SGR/legacy encoding, pixel/cell coordinates, resize,
motion deduplication, tracking modes, modifiers, bounded wheel batches and
focus-loss releases. Right-click still pastes once by default; the widget's
`setRightClickPaste(false)` lets a future native setting send it to a TUI instead.
Shift gestures stay local even if Shift is released before the mouse button.
Actual desktop pointer/DPI behavior, horizontal wheel, selection and real
vim/tmux acceptance remain pending.
