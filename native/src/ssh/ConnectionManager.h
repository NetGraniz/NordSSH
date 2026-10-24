#pragma once
// Qt headers precede libssh's Windows socket headers (min/max macros).
#include "HostResolver.h"
#include <QPointer>
#include "ConnectionAttempt.h"
#include "ByteQueue.h"

namespace nordssh::ssh {
namespace detail { struct ConnectionState; }

struct Credentials final {
    enum class Kind { Password, PrivateKey, Interactive };
    Kind kind;
    vault::SecretBytes secret;
    vault::SecretBytes passphrase{0};
    std::string certificate;
    [[nodiscard]] bool valid() const;
};
struct AuthenticationPrompt {
    AttemptId request, challenge;
    InteractiveChallenge content;
    ConnectionAttempt::Clock::time_point deadline;
};
struct ConnectionStatus {
    std::optional<AttemptId> request;
    AttemptPhase phase = AttemptPhase::Closed;
    AttemptReason reason = AttemptReason::None;
    bool settled = true; // Actual session/key buffers destroyed, not merely cancelled.
    std::size_t pendingOutputBytes = 0;
};

// App-owned registry, outliving tabs. Normal shutdown is asynchronous: call
// shutdown(), keep the event loop alive until idle(), then quit the application.
// Destructor's blocking fallback is for final app teardown, never a tab close.
class ConnectionRuntime final : public QObject {
public:
    static constexpr std::size_t MaxWorkers = 16;
    explicit ConnectionRuntime(QObject* parent = nullptr);
    ~ConnectionRuntime() override;
    void shutdown();
    [[nodiscard]] bool idle() const;
    [[nodiscard]] bool stopping() const;
    [[nodiscard]] std::size_t workers() const;
private:
    friend class ConnectionManager;
    bool reserve();
    void releaseReservation();
    bool launch(const std::shared_ptr<detail::ConnectionState>& state);
    void reap();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One UI-owned connection controller. No credentials in Qt signals/properties,
// no worker callbacks into a tab, and no GUI joins. State/public metadata are
// sampled on a bounded timer; terminal output uses the bounded byte mailbox.
class ConnectionManager final : public QObject {
public:
    explicit ConnectionManager(ConnectionRuntime& runtime, QObject* parent = nullptr);
    ~ConnectionManager() override;
    bool start(const Endpoint& endpoint, const std::optional<TrustedHostPin>& saved = {},
               AttemptTimeouts timeouts = {}, int columns = 80, int rows = 24);
    [[nodiscard]] ConnectionStatus status() const;
    [[nodiscard]] std::optional<HostPrompt> hostPrompt();
    bool approve(AttemptId request, AttemptId challenge);
    bool reject(AttemptId request);
    bool authenticate(AttemptId request, Credentials credentials);
    [[nodiscard]] std::optional<AuthenticationPrompt> authenticationPrompt();
    bool answerAuthentication(AttemptId request, AttemptId challenge, Session::InteractiveAnswers answers);
    bool setKeepAlive(std::chrono::seconds interval);
    bool cancel();
    bool send(std::span<const std::byte> bytes);
    bool resize(int columns, int rows);
    [[nodiscard]] std::optional<vault::SecretBytes> takeOutput();
private:
    void owner() const;
    void poll();
    void stopWorker();
    void deliverApproval(const HostIdentity& identity);
    QPointer<ConnectionRuntime> runtime_;
    HostResolver resolver_;
    QTimer timer_;
    std::unique_ptr<ConnectionAttempt> attempt_;
    std::shared_ptr<detail::ConnectionState> state_;
    Endpoint endpoint_{};
    std::optional<TrustedHostPin> saved_;
    AttemptTimeouts timeouts_;
    int columns_ = 80, rows_ = 24;
    bool seenHost_ = false, seenAuth_ = false, approvalSent_ = false, authSent_ = false;
    bool reserved_ = false;
    std::chrono::seconds keepAlive_{30};
};
} // namespace nordssh::ssh
