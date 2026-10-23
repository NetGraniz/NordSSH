# Native implementation evidence

Date: 2026-10-23. This is an engineering checkpoint, not a release announcement.
The native transport now has isolated SSH password/PTY tests, but there is no
user-facing SSH/SFTP application or on-disk vault service yet.
The user is not asked to test intermediate harnesses or mock interfaces.

## Completed locally

- Verified canonical origin/branch and successful fast-forward pull before work.
- Installed Qt 6.12.0 base/tools/translations SDK; `qmake`, `qtpaths6` and
  `windeployqt` run and report 6.12.0. HTTPS independent mirrors, published SHA1 and
  matching SHA256 pins were checked. These pins are first-acquisition evidence,
  not upstream signed SHA256 attestations. See NATIVE_TOOLCHAIN.md.
- Built native C++20 core with MSVC 19.44.35229 and Windows SDK 10.0.26100.0.
- `vault_format`: passed, fixed header/length/profile/revision admission, strict
  UTF-8 scalar count and seven-symbol minimum, bounded password input, exact AAD.
  This test does not exercise AES-GCM, Argon2, file transactions or JSON parsing.
- `paste_router`: passed with Qt 6.12.0 offscreen, synthetic key/mouse events for
  Ctrl+V, Ctrl+Shift+V, Shift+Insert, Windows RU virtual-key path, key-release/
  autorepeat consumption, Ctrl+C/AltGr/IME separation, parent QShortcut conflict,
  reentrant callback suppression and one right-click action.
  No real clipboard, native keyboard layout, byte-over-SSH or Wayland test yet.
- Read-only security review approved structural admission within its limited
  scope. Added independent exact domain-byte fixtures and boundary regressions.

Latest local command after configuring the Qt input gate:

```text
cmake --build native/build-vault --config Debug --parallel 4
ctest --test-dir native/build-vault -C Debug --output-on-failure
2/2 passed: paste_router (0.23 s), vault_format (0.07 s)
```

Source was an uncommitted working tree at that run; see the enclosing Git commit
for these sources. Raw local logs remain under the ignored build directory.
GitHub native core/sanitizer jobs rerun the committed source independently.
[Run 39957664492](https://github.com/NetGraniz/NordSSH/actions/runs/39957664492)
passed all three jobs for commit `99f0ec689b057679a9711821ad5c1ead135026aa`:
Windows core, Ubuntu 24.04 core and Ubuntu ASan/UBSan. They test vault structural
admission, not Qt input, encryption, real SSH or complete application acceptance.

## Terminal decision

The official libvterm 0.3.3 source compiled with MSVC; a limited happy-path test
passed (0.22 s). Source review then found security-blocking parser/index/numeric
and combining-state paths. The engineering lead rejected production integration.
The experiment is isolated, OFF by default and not linked to product modules.
See NATIVE_TEST_MATRIX.md for scope and nonclaims.

libghostty-vt is the next candidate, not an approved release dependency. Its
Windows feasibility and bounded-memory behavior need their own recorded results.
The official Zig 0.17.0 download was hash-verified; initial test attempts hit Zig
TLS/cache acquisition problems, not failed executed VT assertions. A retry is
being evaluated; neither its runtime test gate nor ReleaseSafe build is yet
recorded as passed. Do not infer success from a running compiler.

## Second checkpoint: authenticated encryption

Implemented `native/src/vault/VaultCrypto.*` and tests, using OpenSSL 3.5.9
and hash-pinned Argon2 20190702. The OpenSSL source archive was checked against
upstream SHA256/GitHub digest and a mathematically verified detached signature;
the signature check is not a full OpenPGP revocation/trust assessment. Acquisition
records are in local `.tools/nordssh-deps/{acquisition,verification}.json`.

Windows MSVC Debug CTest passed `vault_crypto` (6.96 s) and `vault_format`
(0.03 s). Tests used the freshly built OpenSSL source-tree DLL/import library;
the complete OpenSSL installation and packaging gate were not yet complete.

Executed assertions cover:

- Official AES-256-GCM known-answer vectors with and without AAD, including tag
  failure and empty low-level GCM input. Vault payloads cannot be empty.
- RFC 9106 section 5.3 Argon2id vector and the application's fixed 64 MiB/t3/p1
  profile against separately packaged argon2-cffi output.
- Opening a frozen envelope generated independently by argon2-cffi 25.1.0 /
  bindings 26.1.0 and cryptography 50.0.2. The deterministic generator is checked
  in under `native/tests/reference/`; its fixed secrets/nonces are synthetic only.
- Password/key opening, wrong password/key, mutation of every file byte including
  the wrapping tuple during device-key opening, truncation, trailing bytes,
  cross-vault wrapper splicing, revision increment, fresh save nonce and unchanged
  password wrapper during ordinary save.
- Move-only secret ownership; cleansing is implemented with OpenSSL clear/free.
  This is not a claim of protection against crash dumps or same-user malware.

These are in-memory cryptographic envelopes, not a complete vault service. Strict
JSON/schema validation, disk transactions/ACLs/locking, cancellation, RNG/provider
fault injection and device storage still require their own tests. Unauthenticated
plaintext is kept private and cleansed on failure; no JSON is parsed by this layer.

[Native CI run 39996541860](https://github.com/NetGraniz/NordSSH/actions/runs/39996541860)
passed for commit `6d008bb`: Windows/Ubuntu structural core, core sanitizers and
the new Ubuntu authenticated-crypto job. The latter built pinned OpenSSL/Argon2
and ran the crypto tests under ASan/UBSan. This does not close persistence/UI gates.

## Third checkpoint: isolated SSH transport

Built libssh 0.12.2 against OpenSSL 3.5.9, with server, implicit command execution,
GSSAPI, compression, packet capture and debug call tracing disabled. No system SSH
service/account was created or changed. Added a blocking single-owner transport
core intended for a worker thread, not the GUI or a finished ConnectionManager.

Windows MSVC Debug `ssh_transport` passed (4.11 s) against a loopback-only AsyncSSH
2.24.1 fixture with an ephemeral in-memory Ed25519 host key and synthetic password.
Five modes checked: authentication refused before key approval; changed-key
refusal; cross-connection approval replay refusal; wrong password; successful
password/PTY/resize/UTF-8 roundtrip. The server independently counted password
attempts and compared the exact received bytes to reject duplicates. This is not
an OS clipboard/layout test. Public test fingerprints, not passwords, use argv.

Host approvals bind endpoint/user, algorithm, SHA256 and a random connection ID,
then recheck the actual connected key. The persistent known-host/challenge UI
policy is not implemented by this transport and must not be bypassed when wiring
the application. Blocking handshake/auth require the future worker cancellation
and lifetime controller; current tests only establish isolated protocol behavior.

## Fourth checkpoint: VT native ABI baseline

Pinned Ghostty commit `08a9518340e19487bd5fdf96963583fb32f0c41f`, Zig 0.17.0,
MSVC target and `simd=false`: ReleaseSafe SDK completed, **45/45 build steps**.
The independently run upstream core test executable completed with **3220 passed,
35 skipped, 2 failed**. Both failures report `PermissionDenied` while creating
filesystem symlinks in `lib.tinyio` tests. Windows security policy was not changed.
This is a partial result, not a passing full upstream gate. An earlier protocol
run was interrupted for diagnosis; its exit-255 report is not an engine crash.
The separate upstream C API test executable also completed: **3707 passed,
35 skipped, 2 failed**, with the same two symlink-permission failures. These two
executables contain overlapping tests; their counts are not unique coverage.

Our `ghostty_abi` C++/MSVC static-link test passed (0.03 s): fragmented UTF-8,
combining/wide text, alternate screen restore, resize, disabled clipboard hooks,
10,000 output lines under configured history bounds and a limited page-resident
memory check. Its formatter is only a test assertion helper, not a renderer or
text-box substitute. This does not close visual rendering, actual clipboard,
full memory/RSS, mouse/key encoding or fullscreen-program gates.

The manual `native-vt-gate.yml` repeats upstream tests and SDK/C++ ABI checks on
Windows and Ubuntu with pinned source and hash-verified Zig downloads. Results
must be read before marking either platform passed. No app release is generated.

## Latest combined local and CI results

Windows Debug CTest, after the SSH fixture additionally asserted receipt of the
100x40 resize: **5/5 passed** in 10.35 s (`ghostty_abi`, `vault_crypto`,
`ssh_transport`, `paste_router`, `vault_format`). Runtime DLLs came from the
installed Qt/libssh/OpenSSL SDK prefixes; the cached OpenSSL import-library path
still pointed to the matching source build. This is not a clean-machine package.

[Run 39997931879](https://github.com/NetGraniz/NordSSH/actions/runs/39997931879)
passed for `eac6738`, including the Ubuntu crypto/SSH fixture under ASan/UBSan.
[Run 39998899384](https://github.com/NetGraniz/NordSSH/actions/runs/39998899384)
passed the same core/crypto/SSH jobs for `066bac1`. Qt input and Ghostty ABI are
not enabled by these automatic jobs. The manual cross-platform VT run is
[39998901128](https://github.com/NetGraniz/NordSSH/actions/runs/39998901128),
whose terminal tests were still in progress at this checkpoint.

## Next exit criteria

1. A safe pinned VT engine plus Qt raster renderer, real loopback SSH terminal,
   resize/fullscreen programs and byte-level single-paste verification.
2. AES-GCM/Argon2 and persistence tests against the new vault format, then saved
   hosts/auth/trust/SFTP/jump/local-forward integration with synthetic fixtures.
3. Finished native interface and full Windows/Linux installers/portable updates.

Existing prototype vaults, backups, signing keys and production hosts were not
read, changed or used as test data. No native release has been published.

## Fifth checkpoint: Qt raster terminal and real SSH byte path

Resumed after the separate NordQueueTab task. Verified clean canonical main,
origin and a successful fast-forward pull. Added `native/src/terminal` using the
same pinned Ghostty SDK. The engine remains a candidate under the full release
gate; this is further engineering evidence, not a claim of complete approval.

`TerminalModel` owns the engine/render iterators and copies visible cells into a
bounded Qt frame. `TerminalWidget` paints those cells through QPainter's raster
path. It is not QTextEdit, HTML or an ANSI-stripping approximation. Checks cover
fragmented UTF-8, combining/wide cells, truecolor/SGR, alternate screen restore,
normal/application cursor keys, Ctrl+C, Enter, Tab and grid resizing. The adapter
uses the pinned engine's encoder, not a hardcoded set of escape strings.

One PasteRouter serves the widget. Tests cover Ctrl+V, Windows RU virtual-key
events, right click, Shift+Insert, cancelled multiline paste, bracketed paste and
one IME commit. A confirmation uses the captured bytes, not a fresh clipboard
read. A nested confirmation may destroy the tab safely. Remote clipboard and
image/file/shared-memory hooks are disabled. The widget has no process/URL hooks.

Engineering limits: 64 KiB per feed/paste, 512 columns by 256 rows, 4 MiB/10,000
lines of approximate engine scrollback, 256 KiB accumulated reply bytes and 1 MiB
visible UTF-8 cluster bytes per frame. Oversized clusters display a replacement
character rather than forcing unbounded UI allocation. Reply overflow fails
closed. The engine's history byte cap has page-granularity slack. A 20,000-line
test measures resident engine pages below 12 MiB; it is not total process RSS or
a hostile-input memory audit.

The new `terminal_ssh` test runs a real libssh connection on a separate test-only
worker, feeds SSH output to the actual QWidget, sends a synthetic user paste via
Ctrl+V and resizes the PTY to 100x40. The independent AsyncSSH server verifies the
exact received UTF-8 bytes with no extra Enter or duplicate. Trust is checked
against the ephemeral fixture's known SHA256 before authentication; this test
does not introduce a product auto-trust path. No real hosts or credentials used.

This test exposed an idle-read bug: libssh's `ssh_channel_read_timeout()` returns
`SSH_AGAIN` on timeout, which the previous transport treated as failure. Corrected
that case and added an explicit idle-read regression before subsequent writes.
The behavior was checked in the pinned upstream 0.12.2 `src/channels.c`; the web
API documentation timed out during this run. Qt paint/event behavior and the
pinned Ghostty API documentation/source were also checked.

Windows MSVC Debug CTest completed **7/7 passed in 14.62 s**: `ghostty_abi`,
`vault_crypto`, `ssh_transport`, `paste_router`, `terminal_widget`, `terminal_ssh`,
`vault_format`. Tests use the installed development DLLs, not a clean-machine
package. Qt uses `offscreen` and an injected synthetic paste source. A synthetic
raster PNG is retained under the ignored build directory for visual inspection.
An initial screenshot exposed missing offscreen fonts (all text was tofu).
The Windows raster test now loads the machine's existing Consolas font into
that test process and checks actual Latin/Cyrillic glyph availability. Font
changes also recalculate the widget grid/size hint, fixing a stale-metrics case.
The corrected screenshot was visually inspected: Latin/Cyrillic, combining
accent, colors and decorations render. Its CJK character is still a missing-glyph
box with this single test font; CJK font fallback/packaging is not closed. No
Windows fonts were modified or copied into the repository/distribution.
The SSH transport, widget and integrated SSH tests also passed five consecutive
runs each before the font-test extension; this is repetition, not 15 distinct
new acceptance scenarios. The unchanged dependency-free core still passes.
The final combined rerun after the font/failure-path extensions passed **7/7 in
15.70 s**. Only synthetic fixture material and source/docs are staged for Git.

Still not passed: actual Windows RU/EN keyboard/clipboard, X11/Wayland, mouse,
selection/search/scrollback UI, preedit display, cursor shape/blink, synchronized
output, actual vim/nano/top/less/tmux, full parser/resource corpus and package
acceptance. The test-only worker is not the cancellable product connection
manager. Saved trust/authentication dialogs, vault persistence, SFTP, installers
and signed bundle updates remain work ahead. No native user release was made.

## Sixth checkpoint: bounded native history navigation

After completing the separate NordGuard 0.5.6 release, resumed the clean current
NordSSH main with a successful pull. Added engine-owned scrollback navigation to
the raster terminal: wheel/trackpad deltas, Shift+PageUp/PageDown and Shift+Home/End.
The UI does not build a second unbounded text history. Incoming output preserves
the user's viewed position; actual terminal input returns to the active area.
Cancelled paste does not jump the viewport or send bytes.

Partial wheel detents accumulate within a bounded scalar; pixel deltas are used
where available except X11's documented unreliable pixel path. Qt already encodes
natural scrolling in the delta, so it is not inverted twice. Oversized events are
clamped before the engine call. Local navigation press/release pairs never become
remote key bytes, including Kitty release-reporting mode and modifier release.
Remote mouse mode is not reinterpreted as local history unless Shift is held.
Remote mouse reports themselves, scrollbar UI and selection/search are pending.

The new `terminal_history` test checks these behaviors against the actual pinned
engine and QWidget, including history boundaries, primary/alternate separation,
new-output preservation, Ctrl+C return, malicious-sized deltas and no SSH bytes
from history navigation. These are offscreen synthetic events, not desktop wheel,
RU/EN layout, real clipboard or complete fullscreen-program acceptance.

The earlier manual VT run
[39998901128](https://github.com/NetGraniz/NordSSH/actions/runs/39998901128) has
finished. **Windows passed**: upstream core/C API tests reported 41/41 build steps
and 6933 passes/68 skips across overlapping executables, then ReleaseSafe 45/45
and C++ ABI/structural CTest 2/2. Do not sum overlapping upstream tests as unique
coverage. Windows runner results do not erase the earlier local symlink failures.

**Linux did not pass**: the core and C API test executables each failed the same
`lib.tinyio.test.dirRealPathFile edge cases` test, expecting `NameTooLong` for an
eight-byte output buffer but receiving length 8. Source inspection found the
non-libc `/proc/self/fd` readlink fallback returns a truncated buffer length
without reporting truncation. This identifies a concrete dependency issue; it is
not a demonstrated terminal escape or memory overwrite. Our file/image/clipboard
hooks remain disabled. No upstream source/security policy was changed and no
test was suppressed. The Linux SDK did build, but the old workflow then skipped
our ABI test because of the upstream failure. Updated its condition to collect
ABI evidence after a successful SDK while retaining the failed upstream result.
The modified manual workflow has not been rerun at this checkpoint.

The product still has no user-facing native release or installer. Linux terminal
approval remains open, along with the existing vault/session/SFTP/package gates.

The combined Windows MSVC Debug CTest run passed **8/8 in 13.39 s**, including
the new history test and the real loopback SSH byte path. This was against the
local SDK runtime, not a clean-machine package or a Linux GUI run.
After adding cancelled-paste/viewport and focus-change/key-release regressions,
the final combined run also passed **8/8 in 11.70 s**. No existing vault, saved
connection, signing key, user clipboard or production SSH host was used.

## Seventh checkpoint: scheduled native continuation, 2026-10-24

Verified main/origin and completed `git pull --ff-only` before changes. The
automatic native checks for d2f76c1 passed on GitHub. Read the requirements and
ADR again; the client is still an engineering implementation, not a user release.

Added a repository-owned patch for the Linux TinyIo truncation failure. The
patch script requires the exact pinned Ghostty commit and clean tracked files;
it does not alter the original development SDK. A separate detached dependency
worktree is used locally. The readlink semantics were checked against Linux
man-pages; the POSIX site returned HTTP 403 during verification.

The change reads into bounded scratch storage, fails before copying truncated
paths, and retains exact-fit caller buffers. Linux regression checks zero,
short, one-byte-short and exact-fit buffers, including non-cwd directory handles
and untouched guard bytes. The existing upstream regression remains enabled.
The manual cross-platform workflow now applies this patch before compilation.
Linux validation of the fix is pending a new workflow run; no terminal or
application gate is declared complete by the source change alone.

The first local compile caught obsolete Zig array-repeat syntax in the new
test; replaced it with the pinned compiler's `@splat` syntax before continuing
validation. The first dispatched run was cancelled rather than left to consume
runner time on that known compile error. Filtered Windows `RealPath` tests now
pass (69 passes, two Linux-only skips across overlapping executables). These
Windows results do not execute the Linux-specific regression.

The Windows runner then exposed CRLF conversion of the patch file itself,
before compilation. Added an LF checkout attribute for `.patch` files; patch
application continues to use strict whitespace/error checks on both platforms.

## Eighth checkpoint: native mouse reporting

Added the pinned engine's mouse encoder to the Qt adapter, not a handwritten
escape-sequence generator. Terminal output updates its tracking/format mode;
geometry follows the same logical cells as the renderer. Added press/release,
button drag, any-mode hover and bounded vertical wheel batches. Remote modes no
longer silently discard wheel input. Shift remains a local-history override.
Default right-click paste is unchanged and still uses the single paste router;
a native widget preference can instead route right-click to the remote program.

Gesture ownership survives modifier changes; paste/local drags do not become
remote drags halfway through. Focus loss releases tracked remote buttons once.
Pointer values are finite/bounded before the C ABI, wheel batches cap at 32
events and motion within the same cell deduplicates through the engine. Tests
caught that the upstream state-sync helper resets deduplication on every call;
it now runs when terminal output changes, not for each pointer event.

Checked the pinned C headers/example/implementation and official Qt mouse/wheel
event semantics. The new synthetic offscreen test covers SGR, legacy encoding,
pixel/cell geometry, resize, tracking/modifiers, routing and bounds. The real
loopback SSH fixture independently verifies exact click/release/wheel bytes after
one UTF-8 paste. The combined Windows Debug CTest run passed **9/9 in 11.89 s**
before adding the final gesture/lifetime regressions; final rerun follows below.
This uses the development SDK, not a packaged app or real fullscreen TUI.

Horizontal wheel, selection/search, cursor behavior, preedit, desktop RU/EN/DPI,
full application connections, persisted trust/vaults, SFTP and installers remain
open. No native release or user test request was made.

Final Windows combined rerun after outside-press, local-drag and callback/tab
destruction regressions passed **9/9 in 11.82 s**. History, mouse and real SSH
terminal tests each also passed five consecutive runs (repetition, not new
coverage). Linux full upstream/SDK/ABI run 40037749689 is still pending.

## Ninth checkpoint: bounded authenticated payload admission

Added a dependency-free structural scanner for the native vault's already
authenticated JSON. It implements the format's strict UTF-8/escape/integer
grammar, rejects decoded duplicate keys at every depth, checks all seven root
fields and object-only record arrays, and enforces resource limits during
scanning rather than after DOM allocation. It retains bounded field names,
not owned string values or an entire second plaintext/DOM. The RFC 8259 grammar
was checked against the primary RFC.

The API deliberately separates structural acceptance from complete record-schema
acceptance. Only empty schema-1 vaults qualify at this stage. Nonempty records
and settings are marked unreviewed; no host/credential/reference schema or disk
persistence is implied. Crypto APIs still return authenticated bytes, not a
trusted product model. No existing or synthetic vault file was written here.

Tests cover every documented container/resource boundary, malformed encodings,
surrogate pairs, decoded duplicates including escaped root names, unsupported
versions/unknown fields, record-array types, truncations and byte mutations.
An independent Python decoder cross-checks 500 bounded generated documents and
10,000 deterministic mutations (649 admitted in the recorded run). This caught
Windows test stdin's Ctrl+Z text-mode EOF; the test-only harness now reads binary.
These are regression cases, not an exhaustive fuzz campaign/security audit.

Windows MSVC Debug combined CTest passed **11/11 in 13.51 s** with the development
SDK. Linux/sanitizer CI for this new scanner follows after push. SSH real hosts,
user vaults/backups/signing keys and Minecraft were not accessed or changed.

The scanner's GitHub native checks passed on Windows and Ubuntu, including the
Linux ASan/UBSan jobs: run 40038820149 at 52cc0c3. The patched Windows ReleaseSafe
Ghostty SDK also built (45/45 steps); reconfigured the combined C++ build against
that separate SDK and passed **11/11 in 13.86 s**. The original SDK/source remains
unchanged. No WSL Linux distribution or Docker executable was available locally;
Linux evidence comes from the disposable CI environment, not a local Linux GUI.

## Tenth checkpoint: in-memory OpenSSH keys and user certificates

Extended the single-owner transport with explicit private-key authentication
after bound host approval. No key filename, agent, SSH config or implicit prompt
is used. Temporary key/passphrase copies use cleansed move-only buffers; no
credentials are passed through argv, logs or Qt signals. Source/API behavior was
checked against pinned libssh 0.12.2 and the OpenSSH PROTOCOL.key specification.

Before the private-key decoder/KDF, admit only one OpenSSH key: unencrypted or
AES-256-CTR/bcrypt with 1..64 rounds, 16..64-byte salt and <=16 KiB private section.
Validate fixed Ed25519/ECDSA public shapes and RSA 2048..8192-bit modulus with a
bounded exponent. Bind the admitted public hash to the imported private key before
signing. This prevents an attacker from hiding an unbounded KDF cost behind a
small file. It is an initial supported subset, not universal PEM/PKCS8/PPK/cipher
compatibility. The future connection manager still needs process-wide import
scheduling/cancellation; the limit here is per operation, not a total CPU proof.

Optional OpenSSH user certificates must match the private key. Certificate input
is bounded at 64 KiB; raw public keys are not accepted as certificates. Modern
public-key/signature algorithms are explicit; SHA1 ssh-rsa authentication is not
enabled. Host certificates/host-CA trust and keyboard-interactive/2FA remain open.

The isolated fixture verifies real shell echo using generated Ed25519, ECDSA and
RSA keys, encrypted keys and matching user certificates. Negative cases include
untrusted host, wrong passphrase, malformed/weak key, excessive/zero KDF rounds,
embedded NUL, public/private mismatch, certificate/key mismatch and server-side
expired/wrong-principal/unauthorized-key rejection. Synthetic material travels on
binary stdin and is not persisted. Added the missing pinned bcrypt 5.0.0 wheel
only to the isolated test environment/CI; it is not a product dependency.

Windows combined CTest passed **12/12 in 28.82 s** before the last public-mismatch
fixture and certificate-size tightening. Final combined rerun and Linux CI
results follow below. No user-facing native build, installer or release exists.

Final combined Windows rerun passed **12/12 in 28.76 s** against the patched
ReleaseSafe VT SDK. The key fixture now contains 24 positive/negative cases,
including all three ECDSA curves, encrypted RSA and a mismatched outer public
section. Server counters assert zero authentication starts for local/trust/KDF
rejection, and actual server-side authentication for remote rejection cases.

The new key/payload code passed GitHub native checks on Windows and Ubuntu at
78a452c: run 40039888016. The Linux crypto/SSH ASan+UBSan job ran **6/6 in 34.25 s**,
including the 24-case independent key/user-certificate fixture. Qt desktop tests
are not part of that Linux job.

The full patched Linux upstream run in 40037749689 now passes: **41/41 build
steps, 6919 passes/80 skips across overlapping test executables**, including the
original truncation failure and added regression. ReleaseSafe SDK built 45/45.
Our C++ step then exposed a separate integration mistake: CMake searched for
`libghostty-vt-static.a`, while this pinned upstream installs `libghostty-vt.a`
on Unix and `ghostty-vt-static.lib` on Windows. Corrected exact static archive
discovery after checking upstream build.zig/pkg-config generation and CMake docs;
no shared-library fallback or test suppression is introduced.

Added an explicit SDK/ABI-only workflow input, defaulting to full upstream tests,
to verify build-system changes without repeating the unchanged ~30-minute
upstream suite. The new scoped run and old full Windows result follow below;
scoped CI is not presented as a full product or terminal gate.

SDK/ABI-only run 40040999111 passed Windows but exposed a GCC portability issue
once Linux archive discovery worked: the ABI test used `std::uint8_t` without
including `<cstdint>`, accidentally relying on MSVC transitive includes. Added
the explicit include and audited the adapter's integer/size includes as well.
No ABI assertion was removed. Native core/crypto checks at 81b07ad passed in run
40040998462; the next SDK/ABI-only run verifies this final integration fix.
