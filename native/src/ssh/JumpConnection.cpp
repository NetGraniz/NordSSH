#include "JumpConnection.h"
#include <QThread>
namespace nordssh::ssh {
JumpConnection::JumpConnection(ConnectionRuntime& runtime, QObject* parent)
    : QObject(parent), jump_(runtime,this), destination_(runtime,this) {
    timer_.setInterval(10); QObject::connect(&timer_,&QTimer::timeout,this,[this] { poll(); });
}
JumpConnection::~JumpConnection() { timer_.stop(); jump_.cancel(); destination_.cancel(); }
void JumpConnection::owner() const { if (thread() != QThread::currentThread()) throw std::logic_error("Jump controller owner violation"); }
ConnectionManager& JumpConnection::jump() { owner(); return jump_; }
ConnectionManager& JumpConnection::destination() { owner(); return destination_; }
bool JumpConnection::settled() const { owner(); return !running_ && jump_.status().settled && destination_.status().settled; }
bool JumpConnection::start(Endpoint jumpEndpoint, Endpoint target, std::optional<TrustedHostPin> jumpPin,
    std::optional<TrustedHostPin> targetPin, AttemptTimeouts timeouts, std::shared_ptr<SessionWork> work) {
    owner(); if (!settled()) return false;
    const auto canonical = HostResolver::canonicalHost(target.host);
    if (!canonical || !target.port || target.user.empty() || target.user.size() > 128) return false;
    target.host = *canonical;
    auto forwarder = std::make_shared<LocalForwarder>(target.host,target.port);
    if (!jump_.start(jumpEndpoint,jumpPin,timeouts,80,24,forwarder)) return false;
    endpoint_ = std::move(target); pin_ = std::move(targetPin); timeouts_ = timeouts;
    forwarder_ = std::move(forwarder); destinationWork_ = std::move(work);
    waiting_ = running_ = true; targetStarted_ = false; timer_.start(); return true;
}
void JumpConnection::cancel() { owner(); waiting_ = false; jump_.cancel(); destination_.cancel(); }
void JumpConnection::poll() {
    const auto bastion = jump_.status();
    if (waiting_ && bastion.phase == AttemptPhase::Active) {
        const auto forwarding = forwarder_->status();
        if (forwarding.port && !forwarding.stopped) {
            waiting_ = false;
            targetStarted_ = destination_.start(endpoint_,pin_,timeouts_,80,24,destinationWork_,forwarding.port);
            if (!targetStarted_) jump_.cancel();
        }
    }
    if (bastion.settled) { waiting_ = false; if (targetStarted_ && !destination_.status().settled) destination_.cancel(); }
    if (targetStarted_ && destination_.status().settled) jump_.cancel();
    if (!waiting_ && jump_.status().settled && destination_.status().settled) { running_ = false; timer_.stop(); }
}
} // namespace nordssh::ssh
