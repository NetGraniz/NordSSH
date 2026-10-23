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
