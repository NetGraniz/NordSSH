#include "ssh/ConnectionManager.h"
#include <QCoreApplication>
#include <QThread>
#include <cstring>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace nordssh::ssh;
using namespace std::chrono_literals;
static void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
template<class Predicate> void await(Predicate predicate, std::chrono::milliseconds budget = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    require(predicate(), "Async connection test deadline exceeded");
}
static nordssh::vault::SecretBytes secret(std::string_view value) {
    nordssh::vault::SecretBytes bytes(value.size());
    std::memcpy(bytes.bytes().data(), value.data(), value.size()); return bytes;
}
static Credentials password(std::string_view value = "synthetic-ssh-password") {
    return {Credentials::Kind::Password, secret(value), nordssh::vault::SecretBytes(0), {}};
}
static nordssh::vault::SecretBytes readSyntheticFrame(std::size_t maximum) {
    std::array<unsigned char, 4> header{};
    std::cin.read(reinterpret_cast<char*>(header.data()), 4);
    require(std::cin.gcount() == 4, "Missing synthetic key frame");
    std::size_t size = 0;
    for (const auto byte : header) size = (size << 8) | byte;
    require(size <= maximum, "Synthetic key frame exceeds cap");
    nordssh::vault::SecretBytes value(size);
    std::cin.read(reinterpret_cast<char*>(value.bytes().data()), static_cast<std::streamsize>(size));
    require(std::cin.gcount() == static_cast<std::streamsize>(size), "Truncated synthetic key frame");
    return value;
}
int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    try {
        require(argc == 4, "Expected isolated fixture arguments");
        const auto port = std::stoi(argv[1]);
        require(port > 1024 && port <= 65535, "Fixture port out of range");
        const Endpoint endpoint{"LOCALHOST.", static_cast<std::uint16_t>(port), "fixture-user"};
        const std::string mode = argv[3];
        ConnectionRuntime runtime;
        unsigned heartbeats = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
        heartbeat.start(1);
        if (mode == "capacity") {
            std::vector<std::unique_ptr<ConnectionManager>> managers;
            // No event dispatch: reserve before DNS, not after thread creation.
            for (std::size_t i = 0; i < ConnectionRuntime::MaxWorkers; ++i) {
                auto manager = std::make_unique<ConnectionManager>(runtime);
                require(manager->start(endpoint), "Runtime rejected valid reservation");
                managers.push_back(std::move(manager));
            }
            ConnectionManager extra(runtime);
            require(!extra.start(endpoint), "DNS reservations exceeded worker cap");
            managers.clear();
            require(runtime.idle(), "Destroyed pending DNS did not release reservations");
            runtime.shutdown();
            require(!extra.start(endpoint), "Shutdown accepted new session");
        } else {
            auto manager = std::make_unique<ConnectionManager>(runtime);
            AttemptTimeouts timeouts;
            if (mode == "trust-timeout") timeouts.trust = 100ms;
            if (mode == "auth-timeout") timeouts.authentication = 200ms;
            std::optional<TrustedHostPin> pin;
            if (mode == "cached" || mode == "changed") {
                TrustedHostPin value{"localhost", endpoint.port, "ssh-ed25519", {}};
                const std::string hash = argv[2];
                require(hash.size() == 64, "Invalid fixture fingerprint");
                for (std::size_t i = 0; i < 32; ++i) value.sha256[i] = std::byte(std::stoul(hash.substr(i * 2, 2), nullptr, 16));
                if (mode == "changed") value.sha256[0] ^= std::byte{1};
                pin = value;
            }
            require(manager->start(endpoint, pin, timeouts), "Connection request rejected");
            const auto id = *manager->status().request;
            require(!manager->authenticate(id, password()), "Credentials accepted before host verification");
            require(!manager->send(std::as_bytes(std::span("early", 5))), "Input admitted before shell active");
            if (mode == "cancel-dns") {
                require(manager->cancel(), "Pending DNS cancel failed");
                await([&] { return manager->status().settled && manager->status().phase == AttemptPhase::Closed; });
                require(manager->status().reason == AttemptReason::Cancelled, "Cancelled DNS reason overwritten");
            } else if (mode == "destroy-banner") {
                await([&] { return runtime.workers() == 1; });
                const auto start = std::chrono::steady_clock::now();
                manager.reset();
                require(std::chrono::steady_clock::now() - start < 100ms, "Tab destructor joined live SSH worker");
                await([&] { return runtime.idle(); }, 2s);
            } else {
                await([&] { return manager->status().phase != AttemptPhase::Connecting; });
                if (mode == "changed") {
                    require(manager->status().phase == AttemptPhase::Rejected && manager->status().reason == AttemptReason::ChangedKey,
                        "Changed key not blocked");
                    require(!manager->authenticate(id, password()) && !manager->hostPrompt(), "Changed key exposed auth/normal override");
                } else {
                    if (mode != "cached") {
                        const auto prompt = manager->hostPrompt();
                        require(prompt && prompt->identity.endpoint.host == "localhost", "No request-bound canonical host prompt");
                        if (mode == "untrusted" || mode == "destroy-trust") {
                            if (mode == "destroy-trust") {
                                const auto start = std::chrono::steady_clock::now(); manager.reset();
                                require(std::chrono::steady_clock::now() - start < 100ms, "Trust tab close blocked GUI");
                            } else require(manager->reject(id), "Host rejection failed");
                        } else if (mode == "bad-challenge") {
                            auto wrong = prompt->challenge; wrong[0] ^= std::byte{1};
                            require(!manager->approve(id, wrong), "Crosswired challenge accepted");
                            require(!manager->approve(id, prompt->challenge), "Rejected challenge revived");
                        } else if (mode == "trust-timeout") {
                            await([&] { return manager->status().settled; });
                            require(manager->status().reason == AttemptReason::Timeout, "Trust timeout not preserved");
                            require(!manager->approve(id, prompt->challenge), "Expired trust challenge accepted");
                        } else {
                            auto stale = id; stale[0] ^= std::byte{1};
                            require(!manager->approve(stale, prompt->challenge), "Stale request approval accepted");
                            require(manager->approve(id, prompt->challenge), "Host approval failed");
                            require(!manager->approve(id, prompt->challenge), "Host approval replay accepted");
                        }
                    }
                    if (mode == "password" || mode == "cached" || mode == "wrong-password" || mode == "auth-timeout" || mode == "shutdown" || mode == "key" || mode == "reconnect" || mode == "remote-eof" || mode == "flood-cancel") {
                        require(manager->resize(100, 40), "Pending resize not coalesced");
                        require(!manager->resize(0, 10), "Invalid resize admitted");
                        auto credentials = password(mode == "wrong-password" ? "wrong-synthetic-password" : "synthetic-ssh-password");
                        if (mode == "key") credentials = {Credentials::Kind::PrivateKey, readSyntheticFrame(65536),
                            secret("synthetic-key-passphrase"), {}};
                        require(manager->authenticate(id, std::move(credentials)),
                            "Credentials not accepted after approval");
                        require(!manager->authenticate(id, password()), "Duplicate auth command admitted");
                        if (mode == "auth-timeout" || mode == "wrong-password") {
                            const auto before = heartbeats;
                            await([&] { return manager->status().settled; });
                            require(manager->status().phase == AttemptPhase::Failed, "Failed auth became active");
                            if (mode == "auth-timeout") require(manager->status().reason == AttemptReason::Timeout && heartbeats > before + 10,
                                "Stalled auth froze GUI or ignored lifecycle timeout");
                        } else if (mode == "flood-cancel") {
                            await([&] { return manager->status().pendingOutputBytes >= ByteQueue::MaxBytes - ByteQueue::MaxChunk; });
                            require(manager->status().pendingOutputBytes <= ByteQueue::MaxBytes, "Controller output exceeded queue limit");
                            const auto begin = std::chrono::steady_clock::now();
                            manager->cancel();
                            await([&] { return manager->status().settled; }, 750ms);
                            require(std::chrono::steady_clock::now() - begin < 750ms, "Flood starved controller cancellation");
                        } else {
                            await([&] { return manager->status().phase == AttemptPhase::Active || manager->status().settled; });
                            if (manager->status().phase != AttemptPhase::Active) {
                                std::cerr << "Synthetic connection phase/reason: " << static_cast<int>(manager->status().phase)
                                    << '/' << static_cast<int>(manager->status().reason) << '\n';
                                throw std::runtime_error("Fixture did not reach active shell");
                            }
                            std::string output;
                            const auto drain = [&] {
                                while (auto data = manager->takeOutput()) {
                                    output.append(reinterpret_cast<const char*>(data->bytes().data()), data->bytes().size());
                                    require(output.size() <= 65536, "Unbounded fixture output");
                                }
                            };
                            await([&] { drain(); return output.find("fixture-ready") != std::string::npos; });
                            const std::string payload = "once-\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\n";
                            std::vector<std::byte> tooLarge(ByteQueue::MaxChunk + 1);
                            require(!manager->send(tooLarge), "Oversized paste partially admitted");
                            require(manager->send(std::as_bytes(std::span(payload))), "Active terminal send failed");
                            await([&] { drain(); return output.find("echo:" + payload) != std::string::npos; });
                            if (mode == "remote-eof") {
                                await([&] { return manager->status().settled; });
                                require(manager->status().reason == AttemptReason::RemoteClosed, "Orderly EOF was not preserved");
                            } else if (mode == "shutdown") {
                                runtime.shutdown();
                                await([&] { return runtime.idle() && manager->status().settled; });
                                require(!manager->start(endpoint), "Shutdown restarted a session");
                            } else {
                                require(manager->cancel(), "Active close failed");
                                await([&] { return manager->status().settled; });
                                require(manager->status().phase == AttemptPhase::Closed && manager->status().reason == AttemptReason::Cancelled,
                                    "Active cancellation outcome changed");
                                if (mode == "reconnect") {
                                    require(manager->start(endpoint), "Settled manager could not reconnect");
                                    require(manager->status().request != id, "Reconnect reused old request id");
                                    await([&] { return manager->hostPrompt().has_value(); });
                                    const auto prompt = *manager->hostPrompt();
                                    require(!manager->approve(id, prompt.challenge), "Old connection approved a new transport");
                                    require(!manager->takeOutput(), "Old terminal output crossed reconnect boundary");
                                    require(manager->reject(prompt.attempt), "Reconnect rejection failed");
                                }
                            }
                        }
                    }
                }
            }
            if (manager) {
                await([&] { return manager->status().settled; });
                const auto final = manager->status();
                require(!manager->authenticate(id, password()), "Terminal outcome accepted credentials");
                require(manager->status().phase == final.phase && manager->status().reason == final.reason, "Terminal outcome mutated");
            }
            await([&] { return runtime.idle(); });
        }
        runtime.shutdown();
        require(runtime.idle(), "Runtime teardown still has workers");
        std::cout << "Connection manager " << mode << " passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
