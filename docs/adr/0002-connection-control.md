# ADR 0002: connection control substrate

Date: 2026-10-24. Status: implemented engineering substrate, not a finished
ConnectionManager or a user-facing SSH connection flow.

The subsequent controller/session UI is recorded in
[ADR 0003](0003-native-session-controller.md); pending-integration notes below
describe this substrate's original scope.

## Worker operations

The libssh session has one worker owner. Wire operations use nonblocking mode
and bounded poll waits, with a shared absolute deadline and a C++ stop token.
The default operation budget is five seconds; admission rejects budgets over
30 seconds. Poll slices are at most 25 ms. These are scheduling limits, not a
guarantee that arbitrary cryptographic work or OS calls finish within 25 ms.

The only connect entry point accepts a pre-resolved numeric IPv4/IPv6 address.
The logical host/port/user remain attached to the connection identity used for
trust. DNS must be asynchronous and independently cancellable in the future
controller; a hostname is not silently passed into blocking libssh resolution.
Scoped/link-local IPv6 addresses are not admitted by this initial numeric parser.

Password/key auth, PTY creation/resize, writes and reads check cancellation.
Writes retain the existing 64 KiB call cap and flush bounded outgoing data before
returning. Read timeout means idle, not failed; cancellation poisons the session.
Private-key decoding/KDF is still a bounded synchronous worker operation: stop
is checked before import and before authentication, not inside third-party KDF
instructions. A process-wide admission gate now allows at most two SSH key
imports, with cancellable deadline-limited waits in slices up to 25 ms. Its
permit is released before network authentication. This is separate from the
vault KDF's existing two-derivation gate, not a shared total of two crypto jobs.

## UI-owner lifecycle

ConnectionAttempt owns one immutable request id and endpoint, phase/deadline,
public host metadata and stop source. It stores no credential or vault data.
Delayed worker messages include the request id. Host decisions also include a
random one-shot challenge, and expiration is checked at the decision itself,
not only by a timer. A cached pin matches logical host, port, algorithm and hash;
a changed pin rejects without a normal override prompt.

Approvals return the captured actual HostIdentity once. The worker must still
pass it to Session::authorizeHost, which verifies the actual connected key and
connection id again. Lifecycle event methods returning true only mean state
changed; they do not authorize any libssh operation by themselves. Authentication
success must originate from a successful worker operation, not from this model.

Cancel requests worker stop and enters Closing. Late connect/auth/dialog messages
cannot revive it. Worker completion closes it once and preserves cancellation
instead of replacing it with a network error. Terminal outcomes are immutable.
Destruction requests stop as well. Reconnect requires a new lifecycle and ids.

This class stays on its UI owner. Only a copied stop token crosses into the worker.
Host canonicalization, asynchronous DNS and bounded terminal byte queues now
have separate engineering components described below. Bounded control commands,
worker scheduling, native dialogs, retry UI, keepalive and shutdown coordination
remain to implement and integrate.
The synchronous Session API must never be called directly from a GUI handler.

## Bounded shell transport

ByteQueue admits at most 1 MiB, 128 chunks, and 64 KiB per chunk. Both byte and
node limits matter: one-byte messages cannot allocate an unbounded list. Push
rejects the whole chunk before allocation when full; no truncated paste is sent.
Queued bytes use move-only cleansed storage. Close permanently rejects new input
and discards pending chunks. A consumer can additionally own one popped 64 KiB
chunk; the pump has one reusable 64 KiB scratch buffer. These are application
queue limits, not an absolute process-memory limit including libssh/Qt/OS buffers.

ShellPump runs on the Session worker. One step attempts stdout and stderr reads
of at most 64 KiB each, rotating stream priority so one free queue slot cannot
indefinitely starve stderr, then one input write of at most 16 KiB. It does not loop
waiting for a channel window. Before each write, a zero-time flush must clear
previous pending socket output. A full GUI output queue stops all transport
pumping, including flush/write calls which could otherwise receive more data
inside libssh. Cancellation is checked before queue capacity and discards input.
Output already queued on orderly EOF remains available for the GUI to drain.
SSH stdout/stderr ordering is preserved within each stream; merging the two
does not invent a cross-stream ordering guarantee.

This pump is not a background thread or a ConnectionManager. A future scheduler
must wait on idle/backpressure rather than spin; control/trust/cancel delivery
must not share a possibly full data queue. It must own the session through actual
worker teardown before reporting shutdown complete.

## Asynchronous host resolution

HostResolver uses Qt 6.12 QHostInfo::lookupHost on its event-loop owner, not
blocking fromName. It has one active request and an absolute deadline of at most
30 seconds. Literal addresses bypass DNS. Cancel and timeout invalidate an
internal attempt ticket before any later result can be used, even if a caller
reuses its external request id. Every DNS completion rechecks the deadline.
Destroying the resolver aborts its lookup without calling the former owner.
Qt/OS resolution work may finish internally after abort; its stale result cannot
authorize a connection. This is application cancellation, not forcible thread kill.

Canonical host admission lowercases ASCII DNS labels and removes a final root
dot; numeric addresses use Qt's canonical representation. DNS labels are capped
at 63 bytes, input at 253 bytes. Initial scope accepts ASCII/ACE hostnames,
IPv4 and unscoped IPv6; raw Unicode, scoped addresses, URLs, userinfo, controls
and malformed labels are rejected. Canonicalize before constructing the trust
identity/ConnectionAttempt. Returned transport addresses are deduplicated,
unscoped, and capped at eight, examining at most 32 candidates per reply. The DNS response's hostName never replaces the
logical trust name. Address fallback/Happy Eyeballs and UI integration are pending.

Reference: [Qt 6.12 QHostInfo](https://doc.qt.io/qt-6/qhostinfo.html), checked
2026-10-24; nonblocking channel/flush semantics also inspected in pinned libssh
0.12.2 sources. Injected DNS backends are an engineering test seam, not a product
DNS substitute. Their completions are queued and bound to the supplied context.

## Verification scope

Isolated loopback fixtures cover silent-banner cancellation/deadline, stalled
password authentication, pre-cancelled connection/key/password/write, cancelled
idle read, numeric admission and logical-host/key binding. A control signal from
the fixture starts stalled-auth cancellation only after the password request is
observed, avoiding a sleep-based race. An authentication deadline may instead
expire during service negotiation; zero credential packets is a valid outcome.

Lifecycle tests cover stale ids, one-shot decisions, changed pins, delayed timers,
endpoint crosswires, cached approval consumption, cancellation/late messages,
immutable outcomes, owner-thread checks and destruction cancellation. They do
not prove that the not-yet-built async manager, UI or shutdown coordinator works.

Checked against pinned libssh 0.12.2 source and the official session/auth/channel
API references. The published stable web reference labels itself 0.12.0; exact
retry/write semantics were also inspected in the installed 0.12.2 source.
