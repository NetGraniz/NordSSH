#include "ConnectionAttempt.h"
#include <algorithm>
#include <openssl/rand.h>

namespace nordssh::ssh {
namespace {
bool text(std::string_view value, std::size_t cap) {
    return !value.empty() && value.size() <= cap && std::ranges::none_of(value,
        [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
bool terminal(AttemptPhase phase) {
    return phase == AttemptPhase::Closed || phase == AttemptPhase::Failed || phase == AttemptPhase::Rejected;
}
}
AttemptId ConnectionAttempt::randomId() {
    AttemptId result{};
    if (RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), static_cast<int>(result.size())) != 1)
        throw std::runtime_error("Connection request initialization failed");
    return result;
}
ConnectionAttempt::ConnectionAttempt(const Endpoint& endpoint, AttemptTimeouts timeouts, Clock::time_point now)
    : timeouts_(timeouts), id_{}, deadline_(now) {
    if (!text(endpoint.host, 253) || !text(endpoint.user, 128) || !endpoint.port ||
        timeouts.connect.count() < 1 || timeouts.connect > std::chrono::seconds(30) ||
        timeouts.trust.count() < 1 || timeouts.trust > std::chrono::minutes(5) ||
        timeouts.authentication.count() < 1 || timeouts.authentication > std::chrono::minutes(5))
        throw std::invalid_argument("Invalid connection request or timeout limits");
    endpoint_ = endpoint; id_ = randomId(); deadline_ = now + timeouts.connect;
}
ConnectionAttempt::~ConnectionAttempt() { stop_.request_stop(); }
void ConnectionAttempt::owner() const {
    if (owner_ != std::this_thread::get_id()) throw std::logic_error("Connection lifecycle owner violation");
}
AttemptId ConnectionAttempt::id() const { owner(); return id_; }
AttemptPhase ConnectionAttempt::phase() const { owner(); return phase_; }
AttemptReason ConnectionAttempt::reason() const { owner(); return reason_; }
ConnectionAttempt::Clock::time_point ConnectionAttempt::deadline() const { owner(); return deadline_; }
std::stop_token ConnectionAttempt::stopToken() const noexcept { return stop_.get_token(); }
bool ConnectionAttempt::finish(AttemptPhase phase, AttemptReason reason) {
    if (terminal(phase_)) return false;
    phase_ = phase; reason_ = reason; challenge_ = {}; cachedPending_ = false;
    stop_.request_stop(); return true;
}
bool ConnectionAttempt::tick(Clock::time_point now) {
    owner();
    if ((phase_ == AttemptPhase::Connecting || phase_ == AttemptPhase::HostVerification ||
         phase_ == AttemptPhase::Authentication) && now >= deadline_)
        return finish(AttemptPhase::Failed, AttemptReason::Timeout);
    return false;
}
bool ConnectionAttempt::connected(AttemptId request, const HostIdentity& identity,
    const std::optional<TrustedHostPin>& saved, Clock::time_point now) {
    owner(); tick(now);
    if (request != id_ || phase_ != AttemptPhase::Connecting) return false;
    if (identity.endpoint.host != endpoint_.host || identity.endpoint.port != endpoint_.port ||
        identity.endpoint.user != endpoint_.user || !text(identity.algorithm, 128))
        return finish(AttemptPhase::Failed, AttemptReason::Protocol);
    identity_ = identity;
    if (saved) {
        if (saved->host != endpoint_.host || saved->port != endpoint_.port ||
            saved->algorithm != identity.algorithm || saved->sha256 != identity.sha256)
            return finish(AttemptPhase::Rejected, AttemptReason::ChangedKey);
        phase_ = AttemptPhase::Authentication; cachedPending_ = true;
        deadline_ = now + timeouts_.authentication;
    } else {
        challenge_ = randomId(); phase_ = AttemptPhase::HostVerification;
        deadline_ = now + timeouts_.trust;
    }
    return true;
}
std::optional<HostPrompt> ConnectionAttempt::prompt(Clock::time_point now) {
    owner(); tick(now);
    if (phase_ != AttemptPhase::HostVerification) return std::nullopt;
    return HostPrompt{id_, challenge_, *identity_, deadline_};
}
std::optional<HostIdentity> ConnectionAttempt::approve(AttemptId request, AttemptId challenge, Clock::time_point now) {
    owner(); tick(now);
    if (request != id_ || phase_ != AttemptPhase::HostVerification) return std::nullopt;
    if (challenge != challenge_) { finish(AttemptPhase::Rejected, AttemptReason::Rejected); return std::nullopt; }
    challenge_ = {}; approvalConsumed_ = true; phase_ = AttemptPhase::Authentication;
    deadline_ = now + timeouts_.authentication; return identity_;
}
std::optional<HostIdentity> ConnectionAttempt::takeCachedApproval(AttemptId request, Clock::time_point now) {
    owner(); tick(now);
    if (request != id_ || phase_ != AttemptPhase::Authentication || !cachedPending_) return std::nullopt;
    cachedPending_ = false; approvalConsumed_ = true; return identity_;
}
bool ConnectionAttempt::authenticated(AttemptId request, Clock::time_point now) {
    owner(); tick(now);
    if (request != id_ || phase_ != AttemptPhase::Authentication || !approvalConsumed_) return false;
    phase_ = AttemptPhase::Active; return true;
}
bool ConnectionAttempt::reject(AttemptId request) {
    owner();
    if (request != id_ || phase_ != AttemptPhase::HostVerification) return false;
    return finish(AttemptPhase::Rejected, AttemptReason::Rejected);
}
bool ConnectionAttempt::cancel() {
    owner();
    if (terminal(phase_) || phase_ == AttemptPhase::Closing) return false;
    return finish(AttemptPhase::Closing, AttemptReason::Cancelled);
}
bool ConnectionAttempt::workerClosed(AttemptId request) {
    owner();
    if (request != id_ || terminal(phase_)) return false;
    return finish(AttemptPhase::Closed, phase_ == AttemptPhase::Closing ? reason_ : AttemptReason::RemoteClosed);
}
bool ConnectionAttempt::workerFailed(AttemptId request) {
    owner();
    if (request != id_ || terminal(phase_)) return false;
    if (phase_ == AttemptPhase::Closing) return workerClosed(request);
    return finish(AttemptPhase::Failed, AttemptReason::Network);
}
bool ConnectionAttempt::failed(AttemptId request, AttemptReason reason) {
    owner();
    if (request != id_ || (reason != AttemptReason::Network && reason != AttemptReason::Protocol &&
        reason != AttemptReason::Timeout && reason != AttemptReason::Authentication)) return false;
    if (phase_ == AttemptPhase::Closing) return workerClosed(request);
    return finish(AttemptPhase::Failed, reason);
}
} // namespace nordssh::ssh
