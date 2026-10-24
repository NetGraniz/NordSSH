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
Build a fresh exact-pinned Ghostty checkout with the reviewed
[TinyIo patch](patches/README.md); the script refuses tracked local changes.
The manual workflow defaults to full upstream tests. Its explicit
`upstream_tests=false` mode repeats SDK/ABI integration only after build-system
changes; a green scoped run is not a full terminal acceptance report.

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
Actual desktop pointer/DPI behavior, horizontal wheel and real vim/tmux acceptance
remain pending.

`terminal_selection` checks engine-owned local drag selection, forward/reverse
ranges, wide/combining Unicode, soft versus hard wraps, history, resize and screen
switches. Ctrl+Shift+C copies plain selected text; Ctrl+C still interrupts. Shift
starts a local selection when a remote program owns the mouse. Copy has a 1 MiB
UTF-8 limit and never truncates; an empty or oversized selection leaves the
clipboard unchanged. Tests inject a clipboard sink, including a callback which
closes its tab, and check shortcut-release suppression under Kitty mode. No
real OS clipboard or actual EN/RU layout is changed. Word/line gestures,
native desktop clipboard acceptance remains open. Double-click selects a word;
a nearby third click selects a line. Dragging outside the terminal scrolls one
row per 50 ms tick, stopping on release, focus loss, hide or screen invalidation.

`terminal_search` exercises the incremental engine search and native find bar.
Ctrl+Shift+F opens it, Enter/Shift+Enter select older/newer matches, and Escape
closes it without sending a terminal key release. The bar reserves screen space
and reports progress/counts. Literal matching ignores ASCII case only; Unicode
is byte-exact. Queries are capped at 256 UTF-8 bytes. Work advances by one engine
feed and up to four ticks per timer step, rather than a blocking full-history
run. Above 10,000 observed matches the search drops retained results and asks
for a narrower query. This is a retention policy, not a custom allocator ceiling:
an individual engine step can transiently exceed that count. Empty/limited
searches and hidden tabs do not keep the UI timer running. Only the selected
match is highlighted; highlight-all, regex and Unicode case folding are not
implemented. Synthetic offscreen tests do not prove native desktop acceptance.

`terminal_presentation` checks DECSCUSR block/bar/underline cursors, blink and
steady modes, cursor color, bounded local IME composition/cancel/commit and
font fallback with a real CJK glyph. No remote surrounding text is supplied to
the input method. The test requires an installed CJK fallback font; Linux CI
installs fonts-noto-cjk. Actual OS input-method interactions remain separate.

`NORDSSH_BUILD_TUI_TESTS=ON` additionally builds a test-only framed stdin driver.
On Linux, `terminal_real_tui` runs real vim/nano/less/top/tmux on disposable PTYs,
feeding output through the Qt raster adapter and checking editor saves/navigation
and resize. It uses temporary HOME/config/file state, never production SSH or
user profiles. The manual Native Linux terminal engineering workflow repeats
the scenarios with offscreen, xcb/Xvfb and Wayland/headless Weston. This driver
is not a product subprocess backend or a physical keyboard/clipboard test.

The separate `x11_input_fixture.py` checks actual X11/XTest key events and
cross-process QClipboard transfer on an explicitly isolated Xvfb display. It
refuses to run without `NORDSSH_ISOLATED_DESKTOP=1`; never point this fixture at
a user's desktop. Both US and Russian layouts stay configured during group
switching. This does not prove Windows native input or Wayland physical input.

The SSH worker core now admits only a resolved numeric transport address while
retaining the logical endpoint for host trust. Network operations accept a stop
token/deadline and use nonblocking libssh with poll slices up to 25 ms. This is
still a synchronous worker API, not a GUI connection manager. `ssh_cancellation`
checks twelve isolated stall/cancel/deadline modes; `connection_attempt` checks
request-bound one-shot host decisions, cached/changed keys, late messages and
immutable close outcomes. See [ADR 0002](../docs/adr/0002-connection-control.md)
for limits and the remaining async-controller work.

`byte_queue`, `shell_pump` and `key_import_permit` check bounded cleansed byte
mailboxes, cooperative bidirectional SSH transport, cancellation under output
pressure and process-wide SSH key-import admission. The loopback pump fixture
drains 3 MiB of stdout and 2 MiB of input with an independent stderr marker;
the server initially refuses to read input until it has drained its output.
These are worker-component checks, not a completed background connection flow.

`NORDSSH_BUILD_RESOLVER=ON` builds the separate Qt 6.12 Core/Network host resolver
and `host_resolver` test. Deterministic fake DNS completions test stale tickets,
absolute deadlines, filtering and destruction; the actual Qt backend is exercised
against localhost only. The native Linux terminal workflow enables this gate.
