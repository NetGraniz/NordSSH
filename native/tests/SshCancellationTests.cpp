#include "ssh/Session.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <thread>
using namespace std::chrono_literals;
using namespace nordssh::ssh;
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static std::string hex(const std::array<std::byte, 32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    for (auto byte : bytes) { const auto n = std::to_integer<unsigned>(byte); value += digits[n >> 4]; value += digits[n & 15]; }
    return value;
}
template<class Action> void reject(Action action) {
    bool rejected = false;
    try { action(); } catch (const SessionError&) { rejected = true; }
    require(rejected, "Cancelled, expired or invalid operation was accepted");
}
int main(int argc, char** argv) {
    try {
        require(argc == 4, "Expected mode, loopback port and public fingerprint");
        const std::string mode(argv[1]); const int port = std::stoi(argv[2]);
        require(port > 1024 && port <= 65535, "Loopback port outside fixture range");
        const Endpoint endpoint{"fixture.invalid", static_cast<std::uint16_t>(port), "fixture-user"};
        Session client; std::stop_source stop;
        const auto started = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point cancelledAt{};
        if (mode == "invalid-address") {
            reject([&] { client.connectResolved(endpoint, "localhost"); });
        } else if (mode == "pre-cancel-connect") {
            stop.request_stop();
            reject([&] { client.connectResolved(endpoint, "127.0.0.1", {stop.get_token()}); });
        } else if (mode == "past-deadline" || mode == "far-deadline") {
            const auto deadline = started + (mode == "past-deadline" ? -1s : 31s);
            reject([&] { client.connectResolved(endpoint, "127.0.0.1", {{}, deadline}); });
        } else if (mode == "cancel-connect" || mode == "deadline-connect") {
            std::jthread timer([&] { std::this_thread::sleep_for(100ms); stop.request_stop(); });
            const OperationControl control = mode == "cancel-connect" ? OperationControl{stop.get_token()} :
                OperationControl{{}, started + 150ms};
            reject([&] { client.connectResolved(endpoint, "127.0.0.1", control); });
        } else {
            client.connectResolved(endpoint, "127.0.0.1");
            const auto identity = client.hostIdentity();
            require(identity.endpoint.host == "fixture.invalid" && hex(identity.sha256) == argv[3],
                "Resolved transport lost logical host/fingerprint binding");
            client.authorizeHost(identity);
            if (mode == "pre-cancel-auth" || mode == "pre-cancel-key") {
                stop.request_stop();
                if (mode == "pre-cancel-auth") reject([&] { client.authenticatePassword("synthetic-password", {stop.get_token()}); });
                else reject([&] { client.authenticatePrivateKey("not-a-key", {}, {}, {stop.get_token()}); });
            } else if (mode == "cancel-auth" || mode == "deadline-auth") {
                const auto authStarted = std::chrono::steady_clock::now();
                std::jthread timer([&] {
                    if (mode == "cancel-auth") {
                        char command = 0; std::cin.get(command);
                        if (command != 'C') return;
                        cancelledAt = std::chrono::steady_clock::now(); stop.request_stop();
                    }
                });
                const auto control = mode == "cancel-auth" ? OperationControl{stop.get_token()} :
                    OperationControl{{}, authStarted + 700ms};
                reject([&] { client.authenticatePassword("synthetic-password", control); });
                reject([&] { client.openShell(80, 24); });
            } else {
                require(mode == "cancel-read" || mode == "pre-cancel-write", "Unknown cancellation mode");
                client.authenticatePassword("synthetic-password"); client.openShell(80, 24);
                std::string output;
                const auto readyDeadline = std::chrono::steady_clock::now() + 1s;
                while (output.find("ready") == std::string::npos && std::chrono::steady_clock::now() < readyDeadline) {
                    const auto data = client.read(50); output.append(reinterpret_cast<const char*>(data.data()), data.size());
                    require(output.size() <= 65536, "Fixture readiness output exceeds cap");
                }
                require(output.find("ready") != std::string::npos, "Cancellation fixture shell not ready");
                if (mode == "cancel-read") {
                    std::jthread timer([&] { std::this_thread::sleep_for(100ms); stop.request_stop(); });
                    reject([&] { static_cast<void>(client.read(1000, stop.get_token())); });
                } else {
                    stop.request_stop(); const std::string payload("must-not-send");
                    reject([&] { client.write(std::as_bytes(std::span(payload)), {stop.get_token()}); });
                }
            }
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require(elapsed < 3s, "Cancellation/deadline did not return promptly on isolated loopback");
        if (mode == "cancel-auth") require(std::chrono::steady_clock::now() - cancelledAt < 750ms,
            "Authentication cancellation latency exceeded isolated fixture bound");
        std::cout << "SSH control " << mode << " passed\n"; return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
