# ADR 0002: connection control substrate

Date: 2026-10-24. Status: implemented engineering substrate, not a finished
ConnectionManager or a user-facing SSH connection flow.

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
instructions. Process-wide key-import scheduling remains required.

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
Host canonicalization, bounded command/output queues, DNS, worker scheduling,
native dialogs, retry UI, keepalive and shutdown coordination remain to implement.
The synchronous Session API must never be called directly from a GUI handler.

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
