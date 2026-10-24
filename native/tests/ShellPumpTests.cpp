#include "ssh/ShellPump.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
using namespace nordssh::ssh;
static void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    try {
        require(argc == 4, "Expected isolated fixture arguments");
        const int port = std::stoi(argv[1]);
        require(port > 1024 && port <= 65535, "Invalid fixture port");
        const std::string mode = argv[3];
        require(mode == "duplex" || mode == "cancel", "Unknown fixture mode");
        ByteQueue input, output;
        std::atomic<bool> finished{false}, pressure{false};
        std::exception_ptr workerError;
        std::jthread worker([&](std::stop_token stop) {
            try {
                Session session;
                session.connectResolved({"fixture.invalid", static_cast<std::uint16_t>(port), "fixture-user"},
                    "127.0.0.1", {stop, std::chrono::steady_clock::now() + 5s});
                const auto identity = session.hostIdentity();
                std::string actual;
                constexpr char hex[] = "0123456789abcdef";
                for (auto b : identity.sha256) { const auto v = std::to_integer<unsigned>(b); actual += hex[v >> 4]; actual += hex[v & 15]; }
                require(actual == argv[2] && identity.endpoint.host == "fixture.invalid", "Host trust crosswire");
                session.authorizeHost(identity);
                session.authenticatePassword("synthetic-ssh-password", {stop, std::chrono::steady_clock::now() + 5s});
                session.openShell(80, 24, {stop, std::chrono::steady_clock::now() + 5s});
                ShellPump pump(session, input, output);
                while (true) {
                    const auto result = pump.step(stop);
                    if (result == ShellPump::Result::Closed) break;
                    if (result == ShellPump::Result::Blocked) pressure = true;
                    if (result != ShellPump::Result::Progress) std::this_thread::sleep_for(1ms);
                }
            } catch (...) { workerError = std::current_exception(); }
            finished = true; // Session and pump destroyed BEFORE publishing completion.
        });
        const auto deadline = std::chrono::steady_clock::now() + 12s;
        while (!pressure && !finished && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        require(pressure, "Did not exercise output backpressure");
        require(output.usage().bytes <= ByteQueue::MaxBytes && output.usage().chunks <= ByteQueue::MaxChunks,
            "Output exceeded capacity");
        if (mode == "cancel") {
            const auto start = std::chrono::steady_clock::now();
            worker.request_stop();
            while (!finished && std::chrono::steady_clock::now() - start < 750ms) std::this_thread::sleep_for(1ms);
            require(finished, "Output pressure starved cancellation");
            worker.join();
            if (workerError) std::rethrow_exception(workerError);
            require(input.usage().closed, "Input not discarded after cancellation");
        } else {
            std::array<std::byte, ByteQueue::MaxChunk> message{};
            std::fill(message.begin(), message.end(), std::byte{'I'});
            std::size_t sent = 0, received = 0, extended = 0;
            constexpr std::size_t inputTotal = 2 * 1024 * 1024, outputTotal = 3 * 1024 * 1024;
            std::string suffix;
            while ((!finished || output.usage().chunks) && std::chrono::steady_clock::now() < deadline) {
                if (sent < inputTotal && input.tryPush(message)) sent += message.size();
                if (auto chunk = output.tryPop()) {
                    for (const auto byte : chunk->bytes()) {
                        // SSH stdout/stderr have independent buffering; no
                        // cross-stream ordering guarantee is imposed here.
                        if (byte == std::byte{'E'}) { ++extended; continue; }
                        if (received < outputTotal) require(byte == std::byte{'O'}, "Output lost/reordered under pressure");
                        else suffix += static_cast<char>(std::to_integer<unsigned char>(byte));
                        ++received;
                    }
                }
                std::this_thread::sleep_for(1ms);
            }
            require(finished, "Duplex flow control deadlocked");
            worker.join();
            if (workerError) std::rethrow_exception(workerError);
            require(sent == inputTotal && received == outputTotal + 8 && suffix == "pump-ok\n" && extended == 1,
                "Duplex byte counts differ");
        }
        std::cout << "Shell pump " << mode << " passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
