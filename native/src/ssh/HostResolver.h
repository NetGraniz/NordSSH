#pragma once
#include <QHostInfo>
#include <QObject>
#include <QTimer>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nordssh::ssh {
using ResolutionId = std::array<std::byte, 16>;
enum class ResolutionStatus { Success, Invalid, Cancelled, Timeout, NotFound };
struct Resolution {
    ResolutionId request;
    ResolutionStatus status;
    std::string host;
    std::vector<std::string> addresses;
};
// Backend completions must be queued on context's thread and bound to its
// lifetime. Abort need not suppress a result already queued; the controller
// independently checks its attempt token and deadline.
class ResolverBackend {
public:
    virtual ~ResolverBackend() = default;
    virtual int start(const QString& host, QObject* context, std::function<void(QHostInfo)> done) = 0;
    virtual void abort(int lookup) = 0;
};

// One active lookup, owner event-loop thread only; no blocking DNS or SSH.
// Call canonicalHost before constructing a ConnectionAttempt/trust lookup.
// Initial scope: ASCII DNS/ACE labels and unscoped numeric IPv4/IPv6. Unicode
// host input is rejected rather than silently normalizing a trust identity.
class HostResolver final : public QObject {
public:
    using Clock = std::chrono::steady_clock;
    using Completion = std::function<void(Resolution)>;
    explicit HostResolver(QObject* parent = nullptr);
    explicit HostResolver(std::unique_ptr<ResolverBackend> backend, QObject* parent = nullptr);
    ~HostResolver() override;
    [[nodiscard]] static std::optional<std::string> canonicalHost(std::string_view host);
    [[nodiscard]] bool start(ResolutionId request, std::string_view host, Clock::time_point deadline, Completion done);
    bool cancel(ResolutionId request);
    [[nodiscard]] bool busy() const;
private:
    struct Ticket {
        ResolutionId request;
        std::string host;
        Clock::time_point deadline;
        Completion done;
        int lookup = -1;
        bool valid = false;
    };
    void owner() const;
    void begin(const std::shared_ptr<Ticket>& ticket);
    void complete(const std::shared_ptr<Ticket>& ticket, ResolutionStatus status, std::vector<std::string> addresses = {});
    void expire();
    std::unique_ptr<ResolverBackend> backend_;
    std::shared_ptr<Ticket> active_;
    QTimer timer_;
};
} // namespace nordssh::ssh
