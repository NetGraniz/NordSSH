# ADR 0005: dedicated SFTP workers and loopback jump routes

Status: engineering implementation, isolated interoperability acceptance in progress.

ConnectionManager can run a worker-owned SessionWork after the normal host-key
gate and authentication. This is not a second process or a GUI callback. Service
connections cannot consume terminal input. TransferManager uses this route with
its own SSH session, so an SFTP operation cannot block a terminal's transport.

The transfer mailbox admits at most 16 outstanding jobs/results, two executing
SFTP workers process-wide, 64 KiB file chunks, 10,000 directory entries and tree
depth 16. Progress is a coalesced snapshot, not an unbounded signal queue. Links
and special files are not followed. Portable filename admission rejects traversal,
Windows device names/ADS and case-normalization collisions in directory downloads.
The current portable path policy refuses unsupported filenames rather than
silently renaming them. File size is capped at 1 TiB; complete-job deadlines are
explicit (1 second–24 hours). Directory copies are atomic per file, not per tree.

Pinned libssh synchronous metadata requests are not restarted on SSH_AGAIN:
that can duplicate protocol requests. Only the dedicated worker calls libssh.
A bounded watchdog/stop callback may call OS socket shutdown, never libssh or
close the descriptor. Callback registration ends before Session destruction.
After cancellation/timeout the service transport is discarded; pending jobs are
cancelled, and a new authenticated connection is needed. No QThread termination.

Downloads use private QSaveFile staging without direct-write fallback. Uploads
use exclusive random 0600 sibling staging. Confirmed replacement requires POSIX
rename support. libssh 0.12.2 automatically chooses its overwrite extension even
for a new target, so new-file upload instead requires OpenSSH hardlink support:
exclusive link creation cannot clobber a concurrently created destination.
Unsupported servers fail safely; there is no unlink-original fallback. Available
fsync is used before commit. Interrupted remote staging can remain identifiable.

Cancellation before commit preserves the old target. A lost remote commit reply
cannot prove whether the atomic commit happened: CommitUnknown requires refresh,
not automatic retry or a claim that the target was untouched. Same-size/mtime
checks detect ordinary concurrent edits, not malicious same-user/server races.

LocalForwarder listens only on IPv4 loopback, with an ephemeral port by default,
Windows exclusive bind and 16 accepted streams. Cooperative bounded channel I/O
preserves TCP half-close and a remote response after local EOF. Each stream has
two cleansed 16 KiB application buffers, in addition to bounded OS/libssh buffers.
One failed stream does not silently approve another host or launch a shell.

JumpConnection composes two independent ConnectionManagers. The destination's
DNS name is interpreted by the jump server, not resolved locally. A route may
connect TCP only to the owned loopback listener, while destination identity keeps
the original hostname/port/user. Both SSH keys need their own gate; authentication
material never crosses between controllers. Cancelling/closing the destination
also closes its jump route. No ProxyCommand, agent or shell-based bridge.

Primary sources: pinned libssh 0.12.2 sftp.c/channel implementation, official
libssh SFTP/channel API, Qt 6.12.0 qsavefile.cpp, Microsoft shutdown and
SO_EXCLUSIVEADDRUSE documentation. Loopback AsyncSSH tests cover ordinary and
negative behavior; they do not substitute for the separate real OpenSSH gate.
