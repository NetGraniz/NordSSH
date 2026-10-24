#include "ssh/ConnectionAttempt.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace nordssh::ssh;
using namespace std::chrono_literals;
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static const Endpoint endpoint{"unit.invalid", 2222, "fixture-user"};
static HostIdentity identity() {
    HostIdentity value{endpoint, "ssh-ed25519", {}, {}};
    value.sha256.fill(std::byte{0x42}); value.connectionId.fill(std::byte{0x11}); return value;
}
int main() {
    try {
        const auto now = ConnectionAttempt::Clock::now(); const auto host = identity();
        ConnectionAttempt normal(endpoint, {}, now); const auto id = normal.id();
        require(normal.connected(id, host, {}, now), "Connected message publishes prompt");
        const auto prompt = normal.prompt(now);
        require(prompt && prompt->identity.sha256 == host.sha256 && prompt->identity.connectionId == host.connectionId,
            "Prompt captures actual connected key and binding");
        require(!normal.authenticated(id, now), "Cannot activate before host decision");
        require(normal.approve(id, prompt->challenge, now).has_value(), "One-shot approval");
        require(!normal.approve(id, prompt->challenge, now), "Approval cannot replay");
        require(normal.authenticated(id, now) && normal.phase() == AttemptPhase::Active, "Approved authentication activates");
        require(!normal.tick(now + 24h), "Active sessions do not inherit a stale prompt timeout");
        require(normal.cancel() && normal.phase() == AttemptPhase::Closing && normal.stopToken().stop_requested(),
            "Cancel asks worker to stop immediately");
        require(!normal.cancel() && !normal.connected(id, host, {}, now), "Late worker result cannot reopen cancelled attempt");
        require(normal.workerFailed(id) && normal.phase() == AttemptPhase::Closed && normal.reason() == AttemptReason::Cancelled,
            "Cancelled worker error preserves one closed cancellation outcome");
        require(!normal.workerClosed(id) && !normal.authenticated(id, now), "Terminal states are immutable");

        ConnectionAttempt stale(endpoint, {}, now); stale.connected(stale.id(), host, {}, now);
        auto challenge = stale.prompt(now)->challenge;
        require(!stale.approve(id, challenge, now) && stale.phase() == AttemptPhase::HostVerification, "Old dialog attempt cannot approve new connection");
        challenge[0] ^= std::byte{1};
        require(!stale.approve(stale.id(), challenge, now) && stale.phase() == AttemptPhase::Rejected && stale.stopToken().stop_requested(),
            "Wrong challenge fails closed");
        ConnectionAttempt expired(endpoint, {}, now); expired.connected(expired.id(), host, {}, now);
        const auto expiredPrompt = expired.prompt(now);
        require(!expired.approve(expired.id(), expiredPrompt->challenge, now + 121s) && expired.reason() == AttemptReason::Timeout,
            "Approval rechecks deadline even if UI timer was delayed");
        ConnectionAttempt connectTimeout(endpoint, {}, now);
        require(connectTimeout.tick(now + 5s) && !connectTimeout.connected(connectTimeout.id(), host, {}, now + 5s), "Expired connect cannot publish trust prompt");

        const TrustedHostPin saved{endpoint.host, endpoint.port, host.algorithm, host.sha256};
        ConnectionAttempt cached(endpoint, {}, now); cached.connected(cached.id(), host, saved, now);
        require(!cached.prompt(now) && !cached.authenticated(cached.id(), now), "Cached pin is not an unbound trusted bool");
        const auto approved = cached.takeCachedApproval(cached.id(), now);
        require(approved && approved->connectionId == host.connectionId && !cached.takeCachedApproval(cached.id(), now),
            "Cached approval tied to actual attempt and consumed once");
        require(cached.authenticated(cached.id(), now), "Cached verified host can authenticate");
        auto bad = saved; bad.sha256[0] ^= std::byte{1};
        ConnectionAttempt changed(endpoint, {}, now); changed.connected(changed.id(), host, bad, now);
        require(changed.reason() == AttemptReason::ChangedKey && !changed.prompt(now) &&
            !changed.takeCachedApproval(changed.id(), now) && !changed.approve(changed.id(), {}, now), "Changed key has no prompt bypass");
        auto otherHost = host; otherHost.endpoint.user = "different-user";
        ConnectionAttempt crosswired(endpoint, {}, now); crosswired.connected(crosswired.id(), otherHost, {}, now);
        require(crosswired.reason() == AttemptReason::Protocol && crosswired.stopToken().stop_requested(), "Endpoint/user crosswire fails closed");
        ConnectionAttempt authTimeout(endpoint, {}, now); authTimeout.connected(authTimeout.id(), host, saved, now);
        static_cast<void>(authTimeout.takeCachedApproval(authTimeout.id(), now));
        require(!authTimeout.authenticated(authTimeout.id(), now + 121s) && authTimeout.reason() == AttemptReason::Timeout,
            "Late authentication cannot reactivate timed-out attempt");

        std::atomic_bool wrongThread{false};
        std::thread outsider([&] { try { static_cast<void>(cached.phase()); } catch (const std::logic_error&) { wrongThread = true; } });
        outsider.join(); require(wrongThread, "Lifecycle stays on its UI owner");
        std::stop_token lifetime;
        { ConnectionAttempt abandoned(endpoint, {}, now); lifetime = abandoned.stopToken(); }
        require(lifetime.stop_requested(), "Abandoning lifecycle cancels worker token");
        bool rejected = false;
        try { ConnectionAttempt invalid(endpoint, {(std::chrono::milliseconds::max)(), 1s, 1s}, now); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Timeout cap checked before timepoint arithmetic");
        std::cout << "Connection lifecycle trust/cancel/deadline checks passed\n"; return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
