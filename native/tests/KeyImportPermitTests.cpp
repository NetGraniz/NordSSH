#include "ssh/KeyImportPermit.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>

using namespace nordssh::ssh;
using namespace std::chrono_literals;
static void require(bool result) { if (!result) throw std::runtime_error("Key-import admission invariant failed"); }
int main() {
    try {
        auto first = std::make_unique<KeyImportPermit>(OperationControl{});
        auto second = std::make_unique<KeyImportPermit>(OperationControl{});
        std::atomic<bool> rejected{false};
        std::stop_source stop;
        std::jthread cancelled([&] {
            try { const KeyImportPermit blocked({stop.get_token(), std::chrono::steady_clock::now() + 1s}); }
            catch (const SessionError&) { rejected = true; }
        });
        stop.request_stop();
        cancelled.join();
        require(rejected);
        rejected = false;
        const auto begin = std::chrono::steady_clock::now();
        std::jthread timed([&] {
            try { const KeyImportPermit blocked({{}, std::chrono::steady_clock::now() + 50ms}); }
            catch (const SessionError&) { rejected = true; }
        });
        timed.join();
        require(rejected && std::chrono::steady_clock::now() - begin < 750ms);
        first.reset();
        // A cancelled/expired waiter must not leak an acquired permit.
        first = std::make_unique<KeyImportPermit>(OperationControl{});
        first.reset(); second.reset();
        std::atomic<unsigned> active{0}, peak{0};
        std::atomic<bool> failure{false};
        std::vector<std::jthread> workers;
        for (int i = 0; i < 8; ++i) workers.emplace_back([&] {
            try {
                const KeyImportPermit permit({{}, std::chrono::steady_clock::now() + 2s});
                const auto count = ++active;
                auto old = peak.load();
                while (old < count && !peak.compare_exchange_weak(old, count)) {}
                std::this_thread::sleep_for(10ms);
                --active;
            } catch (...) { failure = true; }
        });
        workers.clear();
        require(!failure && active == 0 && peak <= 2 && peak > 0);
        std::cout << "Process-wide SSH key-import cap, cancellation, timeout and permit recovery passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
