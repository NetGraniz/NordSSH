# Native engineering gate: test matrix

Date: 2026-10-23. Scope: the first engineering gate in
[NATIVE_IMPLEMENTATION_PLAN.md](NATIVE_IMPLEMENTATION_PLAN.md), against
[N01–N22](NATIVE_REQUIREMENTS.md) and [ADR 0001](adr/0001-native-client.md).
This is a test specification, not a report that the native client works.

## Status and evidence

All executable cases below are **PLANNED / NOT RUN** at creation of this file.
Reading upstream documentation and reviewing requirements are completed activities,
but do not count as executing these cases. Prototype Rust, browser, Playwright and
Tauri results cannot close native cases.

Each executed case must record: case ID, source commit, dependency-lock hash,
OS/build/session type, command or test name, fixture seed/hash, duration, result
(`passed`, `failed`, `not-run`) and log/artifact path. A skipped or unsupported
case remains `not-run`; an assertion-free launch is not a terminal pass.

Use synthetic data in a fresh temporary directory. Do not inspect production
hosts, real credentials, old vaults, signing keys or the user's clipboard.
Clipboard tests require an isolated desktop account/session with synthetic text.
Any test server binds only to loopback and is stopped by its recorded PID.

### Executed baseline and security no-go: 2026-10-23

QA inspected the actual CTest log
`native/build-core/Testing/Temporary/LastTest.log` and the corresponding
`native/tests/TerminalModelTests.cpp` during the first spike. It records the
Windows x64 Visual Studio 2022 Debug executable `terminal_core_tests.exe`, one
CTest case `terminal_core`, passed at 15:46 local time in 0.22 seconds. Source was
an uncommitted working-tree spike, not an immutable release/tested commit.
The recorded executable was built against the official libvterm 0.3.3 tarball,
whose CMake source pin is
`09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0` (SHA256).

Assertions cover a small happy-path subset: ASCII, one fragmented Cyrillic
codepoint, combining accent, wide cell, truecolor component, alternate-buffer
restore, plain/bracketed model-level paste, Ctrl+C, no OSC52 reply, resize,
oversize rejection and scrollback counters. This is **not** a native widget,
OS-layout/clipboard, real SSH, hostile-parser or full matrix pass. All full cases
in the tables retain `PLANNED / NOT RUN` until independently evidenced.

**Decision: libvterm product integration NO-GO.** The lead confirmed the identified
unguarded CSI numeric/index and split-combining/REP paths in the official fetched
source, not just the archived fork. Those source findings invalidate the idea
that happy-path passes and outer feed limits establish safety. This is a static
security decision, not a claim that an exploit or every suspected hang has been
executed/proven. Do not publish exploit payloads or a product release from this
spike. The baseline is retained as experimental evidence; moving its source into
`experiments/` does not turn it into a product dependency. Engine replacement
must rerun the relevant matrix with its own pinned source and native build.

## Platforms and layers

| Layer | Required environment | What it proves |
| --- | --- | --- |
| Core/model | Windows x64 and Linux x64 | Parser, terminal state, crypto and trust-state assertions without GUI/network |
| Qt widget/input | Windows native desktop; Linux X11 and Wayland | Actual native focus, rendering, shortcuts, IME and clipboard behavior |
| Transport integration | Isolated loopback SSH fixture | Bytes cross SSH once; host verification precedes credentials |
| Clean runtime | Windows 11 x64, Server 2025 Desktop Experience x64, Ubuntu 24.04 x64 | Package uses declared runtime, not installed SDKs or a web engine |

Qt offscreen tests are useful for event routing, not evidence of real Windows
keyboard-layout translation or Wayland clipboard behavior. Xvfb does not close a
Wayland case. Unsupported platform claims are not inferred from a successful build.

## 1. libvterm and terminal model

Harness: feed exact byte arrays to the production terminal adapter, inspect cells,
attributes, cursor, modes, scrollback and outbound replies. Flush damage callbacks
at defined boundaries. A paint snapshot supplements state assertions, not replaces
them. Keep expected state/replies checked in independently of the tested parser.

For each short corpus item, run one whole write, all possible two-part splits,
one-byte writes and fixed-seed random chunks of 1–17 bytes. Compare normalized
final state and concatenated output bytes; callback batching need not be identical.

| ID / requirements | Input/action | Required assertion | Status |
| --- | --- | --- | --- |
| VT01 / N08 | ASCII, CR/LF, tabs, backspace, cursor positioning, erase, insert/delete cells and scroll regions | Golden cells, attributes, cursor and wrap state; no out-of-bounds positions | PLANNED / NOT RUN |
| VT02 / N08 | Cyrillic `Привет, мир`, including every UTF-8 byte split | Exact characters and cursor placement; no lost or duplicated bytes/characters | PLANNED / NOT RUN |
| VT03 / N08 | `A界B`, `e` + U+0301, Cyrillic + combining mark; write wide glyph at right margin | Defined cell widths/continuation cells, correct selection text; no orphan half-glyph after erase/resize | PLANNED / NOT RUN |
| VT04 / N08 | Fragmented CSI/SGR, 256-color and truecolor sequences, OSC terminated by BEL/ST, DCS terminated by ST | Same final state as unsplit input; RGB/attributes asserted, not merely visible | PLANNED / NOT RUN |
| VT05 / N08 | Truncated UTF-8, isolated continuation bytes, invalid scalar encoding, embedded NUL | Safe documented replacement/ignore policy; no crash, memory error or unbounded buffering; valid text after recovery displays | PLANNED / NOT RUN |
| VT06 / N08 | CSI with missing/private/oversized numeric parameters, repeated separators, unsupported final byte; ESC/CAN/SUB at parser boundaries | Bounded handling and documented recovery; no integer overflow or unintended command execution | PLANNED / NOT RUN |
| VT07 / N08 | Unterminated OSC/DCS delivered in 1-byte and 4-KiB chunks; terminate/reset, then display a marker | No growth beyond declared parser cap; recovery policy tested explicitly; no crash or stalled event loop | PLANNED / NOT RUN |
| VT08 / N08 | Write main screen, enter `CSI ? 1049 h`, draw alternate screen, leave with `CSI ? 1049 l`; repeat 100 times | Main buffer restored according to declared semantics; alternate contents do not pollute main scrollback; cursor/modes correct | PLANNED / NOT RUN |
| VT09 / N08 | Resize 80x24 → 120x40 → 1x1 → 80x24 while output/selection/alternate screen are active | Dimensions and indices remain valid; declared reflow policy deterministic; wide/combining cells remain valid; zero/negative/huge requests rejected before allocation | PLANNED / NOT RUN |
| VT10 / N08 | Cursor keys, modifiers, function keys, application-cursor mode, mouse reporting enabled/disabled | Exact outgoing escape bytes for each declared mode; plain selection does not send mouse reports when disabled | PLANNED / NOT RUN |
| VT11 / N08 | Scroll beyond cap; search matches across wrapped rows and Cyrillic text | Oldest lines evicted; search respects configured bounds; selection/clipboard text does not contain escape codes | PLANNED / NOT RUN |
| VT12 / N01,N08 | OSC52 read/write, OSC8 hyperlink and title/control payloads, including fragmentation | No clipboard read ever; no default clipboard write, browser launch, process launch or file access; no rich-text interpretation | PLANNED / NOT RUN |
| VT13 / N08 | Fixed-seed mutation corpus and sanitizer-enabled fuzz smoke over VT01–VT12 | No crash, sanitizer finding, infinite loop or cap bypass; save minimal reproducer and seed on failure | PLANNED / NOT RUN |
| VT14 / N08 | Independent loopback shell runs vim/nano/top/less/tmux; resize and reconnect | Real program behavior, alternate screen and keyboard/mouse work; output transcript alone is not reported as an interactive-program pass | PLANNED / NOT RUN |
| VT15 / N08 | CSI with 15/16/17/100 parameters, semicolon/colon mixtures, one-byte chunks; 9/10/19/20/100 numeric digits in CSI and OSC command | No argument-array overrun, signed overflow or parser state corruption on either LLP64 Windows or LP64 Linux; rejection/recovery spans feed boundaries | PLANNED / NOT RUN |
| VT16 / N08 | One base glyph followed by 100,000 U+0301 marks, each mark in its own feed; repeat as one stream and mixed chunk sizes | Combining storage/work remains capped across feeds; first supported marks preserved under documented policy; no quadratic rescan of an ever-growing retained sequence | PLANNED / NOT RUN |
| VT17 / N08 | On fresh terminal, after reset, and after control-only input, send REP (`CSI 1 b`, `CSI 999 b`) before any printable glyph | No infinite loop or uninitialized glyph access; run first in a subprocess with a strict timeout and sanitizer instrumentation | PLANNED / NOT RUN |

For malformed input, do not assert that arbitrary garbage must render identically
to another emulator. Specify security invariants and the adapter's recovery policy.
For valid input, differential comparison with a pinned reference is additional
evidence, not a replacement for golden assertions.

## 2. Native input: one action, one insertion

Use a byte-collecting transport sink for deterministic component tests, then repeat
through a real loopback SSH channel. The sink is a test double, never presented as
a working SSH session. Test source is clipboard text, not shell echo: server-side
line discipline/echo can conceal duplication or alter CR/LF.

| ID / requirements | Input/action | Required assertion | Status |
| --- | --- | --- | --- |
| IN01 / N09 | EN and RU layout × Ctrl+V, Ctrl+Shift+V, Shift+Insert, right click; one press/release | Exactly one UTF-8 payload, no added CR/LF; event consumed once; no duplicate shortcut/action/context-menu path | PLANNED / NOT RUN |
| IN02 / N09 | Key press + auto-repeat + release for paste shortcuts; focus changes during gesture | Declared one-gesture policy; no second insertion on key release or shortcut-override processing | PLANNED / NOT RUN |
| IN03 / N09 | Cyrillic typing and IME preedit → commit; combining text and supplementary Unicode | Preedit sends no terminal bytes; one commit sends exact UTF-8 once; no key-event plus IME duplication | PLANNED / NOT RUN |
| IN04 / N09 | Terminal Ctrl+C, Ctrl+Shift+C, selection; same shortcuts in host/password fields | Ctrl+C sends interrupt in terminal; copy is separate; field shortcuts do not send terminal input or leak passwords | PLANNED / NOT RUN |
| IN05 / N10 | Bracketed paste off/on, payload with Cyrillic and newline | Off: raw approved payload; on: one `ESC[200~` + payload + one `ESC[201~`; never append Enter; control-byte policy tested | PLANNED / NOT RUN |
| IN06 / N10,N13 | Paste multiline/control text, reject/accept confirmation, then close target tab while dialog is pending | Reject sends zero bytes; accept sends once to original live session only; stale approval cannot target another tab | PLANNED / NOT RUN |
| IN07 / N09 | Empty clipboard, no text MIME, clipboard access unavailable, over-limit payload | No network write; bounded error handling; no browser permission prompt or silent truncation | PLANNED / NOT RUN |
| IN08 / N09,N13 | Native focus/tab navigation at 100/150/200% DPI, EN/RU layout switch during session | Focus visible; shortcuts keep working; no duplicate dispatch or accidental execution of a snippet | PLANNED / NOT RUN |

## 3. Bounded resources and responsiveness

These are proposed **engineering-test limits**, not unmeasured production promises.
Before running, lock screen/scrollback/parser/transport/paste limits in the test
manifest and expose counters/high-water marks. A case cannot pass merely because
the OS did not kill the process. The exact production values belong in the design.

| ID | Workload | Required assertion | Status |
| --- | --- | --- | --- |
| RS01 | Test-configured 80x24 screen, 1,000-line scrollback; stream 1,000,000 short lines without retaining input corpus in RAM | Line/cell cap respected; instrumented retained model bytes bounded after warmup; OS private-bytes/high-water recorded separately | PLANNED / NOT RUN |
| RS02 | Feed 64 MiB of unterminated OSC, then DCS, in repeated fixed-size chunks; configure parser cap 64 KiB or lower | Parser never retains beyond cap; discarded-byte count grows instead; CPU work bounded per consumed input chunk | PLANNED / NOT RUN |
| RS03 | Producer faster than renderer; configured queue cap 1 MiB; 30-second output storm | Queue high-water ≤ cap; backpressure visible; no unbounded Qt queued-signal payloads; no silent byte loss unless explicit disconnect policy | PLANNED / NOT RUN |
| RS04 | Close/recreate terminal and switch alternate screen 100 times after a warmup run | Live object/allocation counters return to baseline; no per-cycle growth; OS allocator retention not mislabeled as a definite leak | PLANNED / NOT RUN |
| RS05 | Output storm, resize and Argon2 work while GUI timer samples event-loop delay | No synchronous KDF/network in UI; proposed gate: maximum observed GUI delay <250 ms on recorded runner, or failure investigated; loaded-runner exception does not become silent pass | PLANNED / NOT RUN |
| RS06 | Huge advertised dimensions/lengths, integer-boundary parameters, cancellation during allocation/work | Validate before allocation; reject invalid sizes; no integer overflow; one terminal completion state | PLANNED / NOT RUN |

## 4. Crypto and vault negatives

These cases close only the new primitive/format gate. They are not a security audit
or evidence that all vault UI/export/device-unlock flows are implemented.

| ID / requirements | Input/action | Required assertion | Status |
| --- | --- | --- | --- |
| CR01 / N14 | AES-256-GCM known-answer fixtures and Argon2id RFC test vectors | Exact independent expected bytes; RFC vector parameters are not confused with the product KDF profile | PLANNED / NOT RUN |
| CR02 / N14,N18 | Flip one bit separately in ciphertext, tag, nonce, authenticated UUID/version/KDF metadata; truncate at every field boundary | Authentication/format failure; no parsed or published plaintext; current file unchanged | PLANNED / NOT RUN |
| CR03 / N16 | 6/7 code-point ASCII/Cyrillic/non-BMP passwords; leading/trailing spaces; composed/decomposed text; wrong password | Defined code-point count, no trim/normalization; correct original password unlocks; wrong password exposes no plaintext | PLANNED / NOT RUN |
| CR04 / N14 | Create separate vaults with same contents/password; save each repeatedly | Independent UUID/DEK/salt/nonce; distinct ciphertext; deterministic RNG injection only in tests, never shipped as a production option | PLANNED / NOT RUN |
| CR05 / N18 | Unknown format, enormous lengths/KDF values, excessive collections, corrupt serialization | Limits checked before large allocations/KDF/plaintext parsing; no overwrite with empty/default vault | PLANNED / NOT RUN |
| CR06 / N14,N18 | Cancel, read-only destination, simulated short write/disk full/failed commit; second writer and external file change | Previous bytes unchanged; no direct-write fallback; stale writer rejected; encrypted temporary/backup policy observed | PLANNED / NOT RUN |
| CR07 / N17 | Missing/corrupt/wrong-device unlock record; unavailable Secret Service | Password unlock remains available; no plaintext unlock file fallback; no auto-reading old prototype storage | PLANNED / NOT RUN |
| CR08 / N14 | Synthetic secret canaries across error/log paths and lock/close | No canary in logs/argv/Qt properties or file metadata; owned secure buffers exercise cleanup hooks; do not claim total process-memory erasure | PLANNED / NOT RUN |

## 5. Host trust and lifecycle negatives

| ID / requirements | Input/action | Required assertion | Status |
| --- | --- | --- | --- |
| TR01 / N05 | Unknown key: accept, reject, close dialog, timeout | Authentication callback/network authentication request count remains zero until matching acceptance; all rejection paths close safely | PLANNED / NOT RUN |
| TR02 / N05,N06 | Known same key, then changed key for same endpoint | Same key proceeds; changed key refuses without offering implicit bypass or overwriting trust; password change does not reset host trust | PLANNED / NOT RUN |
| TR03 / N05 | Two concurrent attempts A/B with distinct keys; late/stale/swapped approvals and reused dialog | Connection ID, challenge, endpoint and exact key bound together; no approval applied twice/to another attempt | PLANNED / NOT RUN |
| TR04 / N05 | Cancel/close while trust or auth worker is pending; delete trust concurrently | Exactly one final state; no use-after-free, hung worker or later authentication; trust rechecked before conflicting commit | PLANNED / NOT RUN |
| TR05 / N05,N13 | Endpoint/banner/key labels containing `<img>`, links and control characters | Plain-text rendering; no network/image/HTML interpretation; escaped/safe presentation without hiding real endpoint | PLANNED / NOT RUN |
| TR06 / N04,N05 | Unsupported host certificate/algorithm; invalid/expired/wrong-principal user certificate on isolated fixture | Explicit refusal or server-auth rejection, never silently treating a host certificate as a raw trusted key; no claim of implemented host-CA policy | PLANNED / NOT RUN |

## Windows automation without a real server

1. Build separate Qt Test executables for TerminalModel/InputRouter/Crypto/Trust.
   Use data-driven cases, QSignalSpy and QTest key/mouse events. Memory byte sink
   captures exact output with no network. Tests run in temporary app-data paths.
2. Run parser corpora and injected failures as CTest tests with timeouts. Repeat
   with available sanitizer configurations; record unsupported sanitizer coverage
   rather than turning its absence into a pass. Windows private-bytes samples and
   internal cap counters are complementary evidence.
3. Test actual Windows keyboard translation in an isolated desktop test account:
   EN/RU layouts, native input events and QClipboard. QTest Key_V alone is not an
   EN/RU layout test. Do not change the user's active layout or clipboard.
4. For transport proof, spawn an isolated test-only SSH server bound to
   127.0.0.1/::1 on an ephemeral port, with newly generated synthetic host keys and
   credentials. Use an independently implemented fixture where practical; verify
   peer bytes/auth-request counters. No installed Windows sshd service, admin
   account or production host is needed. Kill only the recorded fixture process.
5. A synthetic shell/data recorder proves transport bytes, not vim/tmux behavior.
   Run actual Unix interactive applications in a disposable Linux CI environment
   for VT14; report that Windows-to-Linux integration separately from model tests.
6. Clean-runtime VM tests inspect imports, loaded modules and child processes,
   then exercise native UI with WebView unavailable. Compilation or searching
   package filenames alone does not prove absence of runtime dependency.

## Go / no-go

The gate is not complete until applicable terminal/input/resource/crypto/trust
cases have evidence on both Windows and Linux. Initial core-only passes may
justify further engineering, but not a user release or an assertion that honest
input, full SSH compatibility or deployment is solved. A failed libvterm
feasibility case returns to the engine decision; it does not permit a text-box
replacement or a hidden return to WebView. Final release still needs N01–N22,
install/upgrade/rollback/SFTP acceptance beyond this engineering matrix.

## Inspection notes: not upstream findings or executed failures

Read-only inspection also examined an existing archived Neovim fork at
`.tools/libvterm-0.3.3`, outside this repository, origin
`https://github.com/neovim/libvterm.git`, commit
`9d6d2112335080312ef8c36667fa717ded4f7daf`. Its directory name does **not** prove
equivalence to the official upstream 0.3.3 tarball. Do not build or vendor that
fork for the product. The following are concrete inspection leads requiring
verification against the pinned official source and executable regressions:

- `parser.c` increments CSI argument index at separators without an observed
  bounds check before writing; its internal header declares 16 argument slots.
  Numeric CSI `long` and OSC command `int` accumulators use multiplication/addition
  without an observed overflow guard. Exercise VT15 on Windows and Linux.
- `state.c` appends combining-only fragments to a dynamically growing buffer and
  rescans existing content. A per-feed cap does not cap this cross-feed state.
  Exercise VT16 with a live allocation counter, not just a final cell count.
- REP repeats by advancing the cursor by `combine_width`; inspection did not
  establish a nonzero initial/reset value before any printable glyph. VT17 checks
  the suspected zero-progress path; this is not yet a reproduced hang.
- `parser.c` computes a pointer difference involving `string_start` in paths
  where it can be null. Confirm official-source behavior and sanitizer coverage;
  do not infer safety merely from a compiler accepting it.

Before untrusted input reaches the engine, verify fixed-size CSI bounds, checked
numeric conversion and per-sequence limits; also bound combining state and enforce
positive progress. Prefer narrow reviewed source fixes when defects exist in the
pinned engine. Any adapter guard must be streaming-aware across chunks and must
preserve valid VT/UTF-8 semantics, not an ANSI regex or a second terminal parser.
Add regression vectors before claiming a guard fixes the issue.

The implementation target supplied for the initial adapter is 32 MiB scrollback,
1 MiB pending input, at most 512 rows by 1,024 columns, and at most 64 KiB per
engine feed. These cap different resources: none independently bounds CSI
parameter indexing, cumulative combining state or an individual parser loop.
Test exact-limit and limit-plus-one inputs for each, checked allocation arithmetic,
both screen buffers and temporary copies; retained screen memory is measured
separately from the scrollback budget.

## Primary references checked

Checked on 2026-10-23; lock-version documentation must be rechecked when the actual
dependency pin is chosen. These links explain APIs/semantics, not executed results.

- [libvterm upstream](https://www.leonerd.org.uk/code/libvterm/): toolkit-independent
  terminal engine, not a ready-made GUI widget.
- [Qt Test overview](https://doc.qt.io/qt-6/qtest-overview.html).
- [QInputMethodEvent](https://doc.qt.io/qt-6/qinputmethodevent.html): preedit/commit.
- [QClipboard](https://doc.qt.io/qt-6/qclipboard.html): platform clipboard API.
- [OpenSSL EVP encryption](https://docs.openssl.org/3.5/man3/EVP_EncryptInit/).
- [Argon2 RFC 9106 test vectors](https://www.rfc-editor.org/rfc/rfc9106.html#section-5).
- [QSaveFile](https://doc.qt.io/qt-6/qsavefile.html) and
  [QLockFile](https://doc.qt.io/qt-6/qlockfile.html): safe writes and cooperative locks.
- [libssh session sequence](https://api.libssh.org/stable/libssh_tutor_guided_tour.html):
  verify the host before authenticating. NordSSH displays SHA256, not legacy MD5.
