#pragma once
#include <libssh/libssh.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nordssh::ssh {
struct Endpoint { std::string host; std::uint16_t port; std::string user; };
struct HostIdentity {
    Endpoint endpoint;
    std::string algorithm;
    std::array<std::byte,32> sha256;
    std::array<std::byte,16> connectionId;
};
class SessionError final : public std::runtime_error {
public:
    SessionError() : std::runtime_error("SSH operation failed or is not permitted in the current state") {}
};

// Blocking, single-owner transport core; construct/use/destroy on one worker,
// never on the GUI thread. This is not the asynchronous ConnectionManager yet.
// No automatic known_hosts, SSH config, password retention or host-key acceptance.
class Session final {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void connect(const Endpoint& endpoint);
    [[nodiscard]] HostIdentity hostIdentity() const;
    // Caller must complete the application's challenge/changed-key policy first.
    // This rechecks the actual connected key; no unbound bool "trusted" flag.
    void authorizeHost(const HostIdentity& approved);
    void authenticatePassword(std::string_view password);
    void openShell(int columns, int rows);
    void resize(int columns, int rows);
    void write(std::span<const std::byte> bytes);
    [[nodiscard]] std::vector<std::byte> read(int timeoutMilliseconds = 100);
private:
    enum class State { New, Connected, Trusted, Authenticated, Shell, Failed };
    void owner() const;
    void checked(int result);
    void require(State state) const;
    ssh_session session_{};
    ssh_channel channel_{};
    Endpoint endpoint_{};
    std::array<std::byte,16> connectionId_{};
    State state_ = State::New;
    std::thread::id owner_ = std::this_thread::get_id();
};
} // namespace nordssh::ssh
