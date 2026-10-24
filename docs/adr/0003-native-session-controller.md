# ADR 0003: native asynchronous session controller

Date: 2026-10-24. Status: engineering implementation, not the full application
release. Extends [ADR 0002](0002-connection-control.md).

## Ownership and shutdown

ConnectionManager stays on its UI/event-loop owner. HostResolver resolves the
canonical logical host asynchronously. DNS and all numeric address attempts
share one connect deadline; at most eight addresses get portions of the remaining
budget. This is sequential fallback, not Happy Eyeballs. No blocking DNS or
libssh operations run in the GUI.

One QThread owns each Session. An application-owned ConnectionRuntime retains
workers beyond tab lifetime. Closing a tab requests cancellation without joining.
At most 16 attempts, including pending DNS reservations and closing workers, are
admitted per runtime. Production creates one application runtime.

Completion is published after Session/key/credential destruction. The registry
also requires QThread::wait(0) before deleting a thread, accounting for native/TLS
teardown. No terminate or detached threads. Window close requests shutdown and
keeps the Qt event loop alive until the registry is idle. Runtime destruction
has a blocking emergency fallback for final process teardown; it is not the
normal close path. Third-party key decoding remains noninterruptible, but tab
destruction does not wait for that decode in the GUI.

## Bounded commands and data

Each worker has one approval slot, one credential slot and one coalesced size
update. Input/output use ADR 0002's bounded cleansed mailboxes. Cancel never waits
behind output. Secrets do not travel through Qt signals, properties or argv.
The existing private-key admission policy is retained without weaker fallbacks.

The manager samples public metadata every 10 ms instead of posting a Qt event
per output chunk. Idle/backpressured workers wait on a condition variable up to
10 ms, woken by input, output consumption, resize and cancellation. This is not
a zero-wakeup scheduler; socket readiness integration remains a future latency/
CPU improvement, subject to measurements.

ConnectionAttempt consumes request/challenge-bound host approval exactly once.
The worker rechecks actual HostIdentity in Session::authorizeHost before any
credential is used. Changed saved keys reject without a normal override. Auth
success is published only after real authentication and PTY/shell opening.
Wrong credentials require a new connection attempt; stale ids/decisions and
old terminal output do not cross that reconnect boundary.

## Native UI slice

NORDSSH_BUILD_SESSION_UI builds an engineering nordssh_native application with
host/port/user inputs, native tabs and the actual raster VT terminal. ConnectionPane
shows host, port, username, algorithm and SHA256 inside the application using
PlainText labels. "Trust this connection" does not silently persist trust.
Password entry is masked, clears after submission and has visible admission errors.
The controller supports in-memory keys/certificates; the current pane offers
password entry, not a key/vault picker.

The existing one-route paste encoder supplies terminal input. Without bracketed
paste, it converts LF to CR; byte-level fixtures verify exact Cyrillic input once.
No browser clipboard permission is involved. Output work is at most four 64 KiB
chunks per UI timer turn. Orderly EOF leaves queued output for draining; a final
VT query without a live peer does not become a user-input error.

Qt line edits necessarily hold edited text. Submission copies UTF-8 into
SecretBytes, cleanses its temporary byte array and clears the field. This does
not claim Qt's internal text, scrollback or crash dumps are locked/secure memory.

## Verification and remaining gates

Ephemeral loopback AsyncSSH fixtures cover pending DNS capacity/cancel, stalled
banner tab destruction, untrusted/changed/replayed decisions, trust/auth timeout,
wrong password, password/cached/encrypted-key login, resize, exact input,
reconnect, EOF, flooded-output cancellation and runtime shutdown. Native Qt tests
click trust controls, submit a masked password, paste Cyrillic, drain final output
and close an active tab. Synthetic key frames use stdin only in the test driver,
not as a product backend. Executed platform results belong in NATIVE_PROGRESS.md.

Persistent vault/trust management, key-selection UI, keyboard-interactive/2FA,
keepalive, jump hosts, forwarding, SFTP, snippets and installers/updater remain
required. Offscreen tests do not replace physical input/IME or clean-package/
full-window installation and upgrade acceptance. This executable does not read
or migrate prototype vaults and is not the completed native release.

References checked: [Qt QThread](https://doc.qt.io/qt-6/qthread.html), including
wait/finished and TLS completion semantics; [Qt QHostInfo](https://doc.qt.io/qt-6/qhostinfo.html);
[libssh authentication](https://api.libssh.org/stable/group__libssh__auth.html),
and pinned local libssh 0.12.2 source for exact transport/auth behavior.
