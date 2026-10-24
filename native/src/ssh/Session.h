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
#include <chrono>
#include <functional>
#include <stop_token>

namespace nordssh::ssh {
struct Endpoint { std::string host; std::uint16_t port; std::string user; };
struct HostIdentity {
    Endpoint endpoint;
    std::string algorithm;
    std::array<std::byte,32> sha256;
    std::array<std::byte,16> connectionId;
};
struct OperationControl {
    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
};
class SessionError final : public std::runtime_error {
public:
    SessionError() : std::runtime_error("SSH operation failed or is not permitted in the current state") {}
};

// Single-owner transport core; construct/use/destroy on one worker,
// never on the GUI thread. This is not the asynchronous ConnectionManager yet.
// No automatic known_hosts, SSH config, password retention or host-key acceptance.
class Session final {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Resolve asynchronously in the controller; preserve the logical endpoint
    // for trust while connecting only to this validated numeric address.
    void connectResolved(const Endpoint& endpoint, std::string_view address, OperationControl control = {});
    [[nodiscard]] HostIdentity hostIdentity() const;
    // Caller must complete the application's challenge/changed-key policy first.
    // This rechecks the actual connected key; no unbound bool "trusted" flag.
    void authorizeHost(const HostIdentity& approved);
    void authenticatePassword(std::string_view password, OperationControl control = {});
    // In-memory OpenSSH key only; no filename/agent/config or implicit prompts.
    // See KeyMaterial.h for the initial bounded encrypted-key import scope.
    // Optional OpenSSH user certificate, NOT a server-host CA trust policy.
    void authenticatePrivateKey(std::string_view privateKey, std::string_view passphrase = {},
                                std::string_view certificate = {}, OperationControl control = {});
    void openShell(int columns, int rows, OperationControl control = {});
    void resize(int columns, int rows, OperationControl control = {});
    void write(std::span<const std::byte> bytes, OperationControl control = {});
    [[nodiscard]] std::vector<std::byte> read(int timeoutMilliseconds = 100, std::stop_token stop = {});
    [[nodiscard]] bool closed() const;
private:
    enum class State { New, Connected, Trusted, Authenticated, Shell, Failed };
    void owner() const;
    void checked(int result);
    void require(State state) const;
    [[noreturn]] void failed();
    void checkpoint(const OperationControl& control);
    bool waitReady(const OperationControl& control);
    int retry(const std::function<int()>& operation, int again, const OperationControl& control);
    void connectImpl(const Endpoint& endpoint, std::string_view address, const OperationControl& control);
    ssh_session session_{};
    ssh_channel channel_{};
    Endpoint endpoint_{};
    std::array<std::byte,16> connectionId_{};
    State state_ = State::New;
    std::thread::id owner_ = std::this_thread::get_id();
};
} // namespace nordssh::ssh
