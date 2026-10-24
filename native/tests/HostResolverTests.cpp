#include "ssh/HostResolver.h"
#include <QCoreApplication>
#include <QHostAddress>
#include <QPointer>
#include <QThread>
#include <cstdlib>
#include <iostream>

using namespace nordssh::ssh;
using namespace std::chrono_literals;
static void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
template<class Predicate> void await(Predicate predicate) {
    const auto deadline = HostResolver::Clock::now() + 2s;
    while (!predicate() && HostResolver::Clock::now() < deadline) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), "Resolver event deadline exceeded");
}
struct FakeState {
    struct Call { QPointer<QObject> context; std::function<void(QHostInfo)> done; };
    std::vector<Call> calls;
    std::vector<int> aborted;
    void reply(std::size_t index, QHostInfo info) {
        const auto call = calls.at(index);
        if (call.context) QTimer::singleShot(0, call.context, [call, info] { call.done(info); });
    }
};
class FakeBackend final : public ResolverBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    int start(const QString&, QObject* context, std::function<void(QHostInfo)> done) override {
        const int id = static_cast<int>(state_->calls.size());
        state_->calls.push_back({context, std::move(done)});
        return id;
    }
    void abort(int id) override { state_->aborted.push_back(id); }
private:
    std::shared_ptr<FakeState> state_;
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(HostResolver::canonicalHost("Example.COM.") == "example.com", "DNS canonicalization");
        require(HostResolver::canonicalHost("2001:0db8::1") == "2001:db8::1", "IPv6 canonicalization");
        require(HostResolver::canonicalHost("127.0.0.1") == "127.0.0.1", "IPv4 admission");
        for (auto host : {"", "-bad.example", "bad-.example", "bad..example", "host:22", "a/b", "a b",
                          "fe80::1%eth0", "[::1]", "999.999.999.999", "a\n", "\xc3\xa9.example"})
            require(!HostResolver::canonicalHost(host), "Invalid host admitted");
        require(!HostResolver::canonicalHost(std::string(64, 'a') + ".example"), "Overlong label admitted");
        require(!HostResolver::canonicalHost(std::string("a\0b", 3)), "NUL host admitted");
        auto fake = std::make_shared<FakeState>();
        HostResolver resolver(std::make_unique<FakeBackend>(fake));
        ResolutionId first{}, second{};
        second[0] = std::byte{1};
        std::vector<Resolution> results;
        const auto callback = [&](Resolution result) { results.push_back(std::move(result)); };
        require(resolver.start(first, "FIRST.EXAMPLE.", HostResolver::Clock::now() + 1s, callback), "Start failed");
        require(results.empty(), "Start completed synchronously");
        require(!resolver.start(second, "other.example", HostResolver::Clock::now() + 1s, callback), "Concurrent lookup admitted");
        await([&] { return fake->calls.size() == 1; });
        require(!resolver.cancel(second) && resolver.busy(), "Stale cancel affected active attempt");
        require(resolver.cancel(first) && !resolver.busy(), "Cancellation failed");
        require(results.size() == 1 && results.back().status == ResolutionStatus::Cancelled && fake->aborted.size() == 1,
            "Cancel outcome not emitted exactly once");
        // Reuse the same caller id deliberately: internal ticket must still
        // reject a delayed result from the previous attempt.
        require(resolver.start(first, "second.example", HostResolver::Clock::now() + 1s, callback), "Restart failed");
        await([&] { return fake->calls.size() == 2; });
        QHostInfo info;
        QList<QHostAddress> addresses{QHostAddress("127.0.0.1"), QHostAddress("127.0.0.1"), QHostAddress("fe80::1%eth0")};
        for (int i = 1; i <= 12; ++i) addresses.append(QHostAddress(QString("2001:db8::%1").arg(i)));
        info.setAddresses(addresses);
        fake->reply(0, info);
        QCoreApplication::processEvents();
        require(results.size() == 1 && resolver.busy(), "Stale reply revived cancelled attempt");
        fake->reply(1, info);
        await([&] { return results.size() == 2; });
        require(results.back().host == "second.example" && results.back().status == ResolutionStatus::Success &&
            results.back().addresses.size() == 8 && results.back().addresses.front() == "127.0.0.1", "Bounded address filtering failed");
        require(resolver.start(second, "timeout.example", HostResolver::Clock::now() + 20ms, callback), "Timeout start failed");
        await([&] { return results.size() == 3; });
        require(results.back().status == ResolutionStatus::Timeout && !resolver.busy(), "Lookup did not time out");
        fake->reply(2, info);
        QCoreApplication::processEvents();
        require(results.size() == 3, "Timeout produced a second completion");
        // Deadline rechecked in the reply even when the event-loop timer has
        // not run yet. Queue the reply, then stall this test's event loop.
        require(resolver.start(first, "late.example", HostResolver::Clock::now() + 40ms, callback), "Late start failed");
        await([&] { return fake->calls.size() == 4; });
        fake->reply(3, info);
        QThread::msleep(60);
        await([&] { return results.size() == 4; });
        require(results.back().status == ResolutionStatus::Timeout, "Delayed reply bypassed absolute deadline");
        require(resolver.start(first, "invalid host", HostResolver::Clock::now() + 1s, callback), "Invalid start failed");
        await([&] { return results.size() == 5; });
        require(results.back().status == ResolutionStatus::Invalid && fake->calls.size() == 4, "Invalid host reached DNS");
        require(resolver.start(first, "budget.example", HostResolver::Clock::time_point::max(), callback), "Budget start failed");
        await([&] { return results.size() == 6; });
        require(results.back().status == ResolutionStatus::Invalid, "Unbounded deadline admitted");
        require(resolver.start(first, "past.example", HostResolver::Clock::time_point::min(), callback), "Past start failed");
        await([&] { return results.size() == 7; });
        require(results.back().status == ResolutionStatus::Timeout && fake->calls.size() == 4, "Expired request reached DNS");
        QHostInfo missing;
        missing.setError(QHostInfo::HostNotFound);
        require(resolver.start(first, "missing.example", HostResolver::Clock::now() + 1s, callback), "Missing start failed");
        await([&] { return fake->calls.size() == 5; });
        fake->reply(4, missing);
        await([&] { return results.size() == 8; });
        require(results.back().status == ResolutionStatus::NotFound, "DNS error not reported");
        {
            auto limitedState = std::make_shared<FakeState>();
            HostResolver limited(std::make_unique<FakeBackend>(limitedState));
            std::optional<Resolution> bounded;
            require(limited.start(first, "bounded.example", HostResolver::Clock::now() + 1s,
                [&](Resolution result) { bounded = std::move(result); }), "Bounded lookup start failed");
            await([&] { return limitedState->calls.size() == 1; });
            QList<QHostAddress> many;
            for (int i = 0; i < 32; ++i) many.append(QHostAddress("fe80::1%eth0"));
            many.append(QHostAddress("127.0.0.1"));
            QHostInfo oversized;
            oversized.setAddresses(many);
            limitedState->reply(0, oversized);
            await([&] { return bounded.has_value(); });
            require(bounded->status == ResolutionStatus::NotFound && bounded->addresses.empty(),
                "Resolver scanned beyond the candidate work cap");
        }
        // Actual Qt async resolver uses localhost only, not public DNS/SSH.
        HostResolver actual;
        require(actual.start(second, "localhost", HostResolver::Clock::now() + 1s, callback), "Qt lookup failed");
        await([&] { return results.size() == 9; });
        require(results.back().status == ResolutionStatus::Success && !results.back().addresses.empty(), "Localhost not resolved");
        auto disposable = std::make_unique<HostResolver>(std::make_unique<FakeBackend>(fake));
        require(disposable->start(first, "destroy.example", HostResolver::Clock::now() + 1s, callback), "Destruction start failed");
        await([&] { return fake->calls.size() == 6; });
        disposable.reset();
        fake->reply(5, info);
        QCoreApplication::processEvents();
        require(results.size() == 9, "Callback after resolver destruction");
        auto selfDeleting = std::make_unique<HostResolver>();
        require(selfDeleting->start(first, "::1", HostResolver::Clock::now() + 1s,
            [&](Resolution result) { require(result.status == ResolutionStatus::Success, "Literal failed"); selfDeleting.reset(); }),
            "Literal start failed");
        await([&] { return !selfDeleting; });
        std::cout << "Async DNS, canonicalization, bounded results, deadline, stale replies and lifetimes passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
