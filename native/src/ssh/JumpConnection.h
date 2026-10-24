#pragma once
#include "LocalForwarder.h"
namespace nordssh::ssh {
// UI-owner composition of two independent trust/auth controllers. The jump
// password/key is never offered to the destination, or vice versa.
class JumpConnection final : public QObject {
public:
    explicit JumpConnection(ConnectionRuntime&, QObject* parent = nullptr);
    ~JumpConnection() override;
    bool start(Endpoint jump, Endpoint destination,
               std::optional<TrustedHostPin> jumpPin = {}, std::optional<TrustedHostPin> destinationPin = {},
               AttemptTimeouts timeouts = {}, std::shared_ptr<SessionWork> destinationWork = {});
    void cancel();
    [[nodiscard]] bool settled() const;
    [[nodiscard]] ConnectionManager& jump();
    [[nodiscard]] ConnectionManager& destination();
private:
    void owner() const;
    void poll();
    ConnectionManager jump_, destination_;
    QTimer timer_;
    Endpoint endpoint_;
    std::optional<TrustedHostPin> pin_;
    AttemptTimeouts timeouts_;
    std::shared_ptr<LocalForwarder> forwarder_;
    std::shared_ptr<SessionWork> destinationWork_;
    bool running_ = false, waiting_ = false, targetStarted_ = false;
};
} // namespace nordssh::ssh
