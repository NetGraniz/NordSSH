#pragma once
#include "ConnectionManager.h"
#include <atomic>
namespace nordssh::ssh {
struct ForwardStatus { std::uint16_t port; unsigned connections; bool stopped; };
class LocalForwarder final : public SessionWork {
public:
    // IPv4 loopback only. Zero chooses an available local port. No shell/config
    // execution, agent forwarding or hidden wildcard listener.
    LocalForwarder(std::string destination, std::uint16_t remotePort, std::uint16_t localPort = 0);
    [[nodiscard]] ForwardStatus status() const;
    void run(Session&, std::stop_token) override;
private:
    std::string destination_;
    std::uint16_t remotePort_, requestedPort_;
    std::atomic<std::uint16_t> boundPort_{0};
    std::atomic<unsigned> connections_{0};
    std::atomic<bool> used_{false}, stopped_{false};
};
} // namespace nordssh::ssh
