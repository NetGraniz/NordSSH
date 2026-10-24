#pragma once
#include "Session.h"
#include <optional>

namespace nordssh::ssh {
using AttemptId = std::array<std::byte, 16>;
enum class AttemptPhase { Connecting, HostVerification, Authentication, Active, Closing, Closed, Failed, Rejected };
enum class AttemptReason { None, Cancelled, Timeout, Network, Protocol, ChangedKey, Rejected, RemoteClosed, Authentication };
struct AttemptTimeouts {
    std::chrono::milliseconds connect{5000}, trust{120000}, authentication{120000};
};
struct TrustedHostPin {
    std::string host;
    std::uint16_t port{};
    std::string algorithm;
    std::array<std::byte, 32> sha256{};
};
struct HostPrompt {
    AttemptId attempt, challenge;
    HostIdentity identity;
    std::chrono::steady_clock::time_point deadline;
};

// UI-owner lifecycle gate for the future ConnectionManager. No network, vault,
// credential storage or UI callbacks. Every delayed message carries an attempt
// id; decisions also carry a one-shot challenge. Not a working session manager.
class ConnectionAttempt final {
public:
    using Clock = std::chrono::steady_clock;
    explicit ConnectionAttempt(const Endpoint& endpoint, AttemptTimeouts timeouts = {}, Clock::time_point now = Clock::now());
    ~ConnectionAttempt();
    ConnectionAttempt(const ConnectionAttempt&) = delete;
    ConnectionAttempt& operator=(const ConnectionAttempt&) = delete;
    [[nodiscard]] AttemptId id() const;
    [[nodiscard]] AttemptPhase phase() const;
    [[nodiscard]] AttemptReason reason() const;
    [[nodiscard]] Clock::time_point deadline() const;
    [[nodiscard]] std::stop_token stopToken() const noexcept; // Copy before starting worker.
    // Event methods report a state change, NEVER authorization by themselves.
    bool connected(AttemptId request, const HostIdentity& identity,
                   const std::optional<TrustedHostPin>& saved = {}, Clock::time_point now = Clock::now());
    [[nodiscard]] std::optional<HostPrompt> prompt(Clock::time_point now = Clock::now());
    [[nodiscard]] std::optional<HostIdentity> approve(AttemptId request, AttemptId challenge, Clock::time_point now = Clock::now());
    [[nodiscard]] std::optional<HostIdentity> takeCachedApproval(AttemptId request, Clock::time_point now = Clock::now());
    bool authenticated(AttemptId request, Clock::time_point now = Clock::now());
    bool reject(AttemptId request);
    bool cancel();
    bool workerClosed(AttemptId request);
    bool workerFailed(AttemptId request);
    bool failed(AttemptId request, AttemptReason reason);
    bool tick(Clock::time_point now = Clock::now());
private:
    void owner() const;
    bool finish(AttemptPhase phase, AttemptReason reason);
    static AttemptId randomId();
    Endpoint endpoint_;
    AttemptTimeouts timeouts_;
    AttemptId id_, challenge_{};
    AttemptPhase phase_ = AttemptPhase::Connecting;
    AttemptReason reason_ = AttemptReason::None;
    Clock::time_point deadline_;
    std::optional<HostIdentity> identity_;
    bool cachedPending_ = false, approvalConsumed_ = false;
    std::stop_source stop_;
    std::thread::id owner_ = std::this_thread::get_id();
};
} // namespace nordssh::ssh
