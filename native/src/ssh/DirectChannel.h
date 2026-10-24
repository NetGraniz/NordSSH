#pragma once
#include "Session.h"
namespace nordssh::ssh {
class DirectChannel final {
public:
    DirectChannel(Session&, const std::string& host, std::uint16_t port, OperationControl);
    ~DirectChannel();
    DirectChannel(const DirectChannel&) = delete;
    DirectChannel& operator=(const DirectChannel&) = delete;
    [[nodiscard]] std::size_t read(std::span<std::byte>);
    [[nodiscard]] std::size_t write(std::span<const std::byte>);
    [[nodiscard]] bool sendEof();
    [[nodiscard]] bool eof() const;
private:
    Session& session_;
    ssh_channel channel_{};
};
} // namespace nordssh::ssh
