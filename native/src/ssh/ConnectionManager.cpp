#include "ConnectionManager.h"
#include "ShellPump.h"
#include <QThread>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <openssl/rand.h>

namespace nordssh::ssh {
namespace detail {
struct ConnectionState {
    Endpoint endpoint;
    std::vector<std::string> addresses;
    ConnectionAttempt::Clock::time_point connectDeadline;
    AttemptTimeouts timeouts;
    std::stop_source stop;
    ByteQueue input, output;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::optional<HostIdentity> host, approval;
    std::optional<Credentials> credentials;
    AttemptId request{};
    std::optional<AuthenticationPrompt> interactive;
    std::optional<Session::InteractiveAnswers> answers;
    std::chrono::seconds keepAlive{30};
    std::shared_ptr<SessionWork> work;
    std::optional<std::uint16_t> loopbackTunnel;
    std::optional<std::pair<int, int>> dimensions;
    ConnectionAttempt::Clock::time_point authenticationDeadline{};
    bool authenticated = false;
    AttemptReason failure = AttemptReason::None;
    std::atomic<bool> finished{false};
    void cancel() {
        stop.request_stop();
        input.close();
        { std::lock_guard lock(mutex); credentials.reset(); approval.reset(); answers.reset(); interactive.reset(); }
        wake.notify_all();
    }
};
}
namespace {
using Clock = ConnectionAttempt::Clock;
using namespace std::chrono_literals;
bool dimensionsValid(int columns, int rows) { return columns >= 1 && columns <= 1024 && rows >= 1 && rows <= 512; }
bool terminal(AttemptPhase phase) {
    return phase == AttemptPhase::Closed || phase == AttemptPhase::Rejected || phase == AttemptPhase::Failed;
}
std::string_view text(const vault::SecretBytes& secret) {
    const auto bytes = secret.bytes();
    return bytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
OperationControl operation(const std::shared_ptr<detail::ConnectionState>& state, Clock::time_point deadline) {
    return {state->stop.get_token(), (std::min)(deadline, Clock::now() + 10s)};
}
void runConnection(const std::shared_ptr<detail::ConnectionState>& state) noexcept {
    AttemptReason failure = AttemptReason::Network;
    try {
        // All libssh construction, operation, key destruction and teardown stay
        // inside this scope, on this worker. Never capture the manager/tab.
        std::unique_ptr<Session> session;
        for (std::size_t index = 0; index < state->addresses.size(); ++index) {
            if (state->stop.stop_requested() || Clock::now() >= state->connectDeadline) throw SessionError();
            auto candidate = std::make_unique<Session>();
            const auto now = Clock::now();
            const auto remaining = state->connectDeadline - now;
            const auto share = remaining / static_cast<Clock::duration::rep>(state->addresses.size() - index);
            const auto budget = (std::min)(state->connectDeadline, now + (std::max)(share,
                std::chrono::duration_cast<Clock::duration>(250ms)));
            try {
                if (state->loopbackTunnel) candidate->connectLoopbackTunnel(state->endpoint, *state->loopbackTunnel, {state->stop.get_token(), budget});
                else candidate->connectResolved(state->endpoint, state->addresses[index], {state->stop.get_token(), budget});
                session = std::move(candidate);
                break;
            } catch (const SessionError&) {
                if (state->stop.stop_requested()) throw;
            }
        }
        if (!session) throw SessionError();
        const auto trustDeadline = Clock::now() + state->timeouts.trust;
        {
            std::unique_lock lock(state->mutex);
            state->host = session->hostIdentity();
            if (!state->wake.wait_until(lock, state->stop.get_token(), trustDeadline,
                    [&] { return state->approval.has_value(); })) { failure = AttemptReason::Timeout; throw SessionError(); }
            const auto approval = std::move(*state->approval);
            state->approval.reset();
            lock.unlock();
            if (state->stop.stop_requested()) throw SessionError();
            session->authorizeHost(approval);
        }
        failure = AttemptReason::Authentication;
        {
            std::unique_lock lock(state->mutex);
            const auto deadline = state->authenticationDeadline;
            if (!state->wake.wait_until(lock, state->stop.get_token(), deadline,
                    [&] { return state->credentials.has_value(); })) { failure = AttemptReason::Timeout; throw SessionError(); }
            auto credentials = std::move(*state->credentials);
            state->credentials.reset();
            lock.unlock();
            const auto control = operation(state, deadline);
            if (credentials.kind == Credentials::Kind::Password)
                session->authenticatePassword(text(credentials.secret), control);
            else if (credentials.kind == Credentials::Kind::PrivateKey)
                session->authenticatePrivateKey(text(credentials.secret), text(credentials.passphrase), credentials.certificate, control);
            else session->authenticateInteractive([&](const InteractiveChallenge& challenge) {
                AuthenticationPrompt prompt{state->request, {}, challenge, deadline};
                if (RAND_bytes(reinterpret_cast<unsigned char*>(prompt.challenge.data()), 16) != 1) throw SessionError();
                std::unique_lock promptLock(state->mutex);
                state->interactive = std::move(prompt);
                if (!state->wake.wait_until(promptLock, state->stop.get_token(), deadline,
                    [&] { return state->answers.has_value(); })) throw SessionError();
                auto answers = std::move(*state->answers);
                state->answers.reset(); state->interactive.reset();
                return answers;
            }, {state->stop.get_token(), deadline});
        } // All password/key material released before the shell loop.
        failure = AttemptReason::Network;
        if (state->work) {
            { std::lock_guard lock(state->mutex); state->authenticated = true; }
            state->work->run(*session, state->stop.get_token());
        } else {
        std::pair<int, int> dimensions;
        {
            std::lock_guard lock(state->mutex);
            dimensions = *state->dimensions;
            state->dimensions.reset();
        }
        session->openShell(dimensions.first, dimensions.second, operation(state, Clock::now() + 5s));
        { std::lock_guard lock(state->mutex); state->authenticated = true; }
        ShellPump pump(*session, state->input, state->output);
        auto lastKeepAlive = Clock::now();
        while (!state->stop.stop_requested()) {
            std::optional<std::pair<int, int>> resize;
            {
                std::lock_guard lock(state->mutex);
                resize = std::exchange(state->dimensions, {});
            }
            if (resize) session->resize(resize->first, resize->second, operation(state, Clock::now() + 2s));
            const auto result = pump.step(state->stop.get_token());
            if (result == ShellPump::Result::Closed) break;
            std::chrono::seconds interval;
            { std::lock_guard lock(state->mutex); interval = state->keepAlive; }
            if (interval.count() > 0 && Clock::now() - lastKeepAlive >= interval && state->output.roomForChunk()) {
                session->keepAlive(operation(state, Clock::now() + 2s));
                lastKeepAlive = Clock::now();
            }
            if (result != ShellPump::Result::Progress) {
                std::unique_lock lock(state->mutex);
                // GUI input/pop/resize/cancel wakes immediately; idle network
                // readiness is sampled at 10 ms, not by a hot retry loop.
                state->wake.wait_for(lock, 10ms);
            }
        }
        }
        failure = AttemptReason::None;
    } catch (...) {
        if (state->stop.stop_requested()) failure = AttemptReason::None;
    }
    // Session, pump, keys and local credentials are destroyed before publishing.
    state->input.close();
    {
        std::lock_guard lock(state->mutex);
        state->credentials.reset(); state->approval.reset(); state->interactive.reset(); state->answers.reset();
        state->failure = failure;
    }
    state->finished.store(true, std::memory_order_release);
}
class SessionThread final : public QThread {
public:
    explicit SessionThread(std::shared_ptr<detail::ConnectionState> state) : state_(std::move(state)) {}
protected:
    void run() override { runConnection(state_); }
private:
    std::shared_ptr<detail::ConnectionState> state_;
};
}
bool Credentials::valid() const {
    const auto value = text(secret), phrase = text(passphrase);
    if (kind == Kind::Interactive) return value.empty() && phrase.empty() && certificate.empty();
    if (value.empty() || value.find('\0') != std::string_view::npos || phrase.size() > 4096 ||
        phrase.find('\0') != std::string_view::npos || certificate.size() > 65536 || certificate.find('\0') != std::string::npos) return false;
    if (kind == Kind::Password) return value.size() <= 4096 && phrase.empty() && certificate.empty();
    return kind == Kind::PrivateKey && value.size() <= 1048576;
}
struct ConnectionRuntime::Impl {
    struct Worker { std::shared_ptr<detail::ConnectionState> state; std::unique_ptr<SessionThread> thread; };
    std::vector<Worker> workers;
    QTimer timer;
    bool stopping = false;
    std::size_t reserved = 0;
};
ConnectionRuntime::ConnectionRuntime(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    QObject::connect(&impl_->timer, &QTimer::timeout, this, [this] { reap(); });
    impl_->timer.setInterval(10);
}
ConnectionRuntime::~ConnectionRuntime() {
    shutdown();
    // Emergency app-teardown fallback only. Normal UI keeps the event loop
    // alive until idle(), so there is no network/KDF wait here in normal use.
    for (const auto& worker : impl_->workers) worker.thread->wait();
}
bool ConnectionRuntime::stopping() const {
    if (thread() != QThread::currentThread()) throw std::logic_error("Runtime owner violation");
    return impl_->stopping;
}
bool ConnectionRuntime::idle() const { return workers() == 0 && impl_->reserved == 0; }
std::size_t ConnectionRuntime::workers() const {
    if (thread() != QThread::currentThread()) throw std::logic_error("Runtime owner violation");
    return impl_->workers.size();
}
bool ConnectionRuntime::reserve() {
    reap();
    if (impl_->stopping || impl_->workers.size() + impl_->reserved >= MaxWorkers) return false;
    ++impl_->reserved; return true;
}
void ConnectionRuntime::releaseReservation() {
    if (impl_->reserved == 0) throw std::logic_error("Runtime reservation mismatch");
    --impl_->reserved;
}
void ConnectionRuntime::shutdown() {
    if (thread() != QThread::currentThread()) throw std::logic_error("Runtime owner violation");
    impl_->stopping = true;
    for (const auto& worker : impl_->workers) worker.state->cancel();
}
bool ConnectionRuntime::launch(const std::shared_ptr<detail::ConnectionState>& state) {
    if (thread() != QThread::currentThread()) throw std::logic_error("Runtime owner violation");
    reap();
    releaseReservation();
    if (impl_->stopping || impl_->workers.size() >= MaxWorkers) return false;
    auto worker = std::make_unique<SessionThread>(state);
    auto* thread = worker.get();
    impl_->workers.push_back({state, std::move(worker)});
    thread->start();
    impl_->timer.start();
    return true;
}
void ConnectionRuntime::reap() {
    for (const auto& worker : impl_->workers) {
        if (!worker.thread->isRunning() && !worker.state->finished.load(std::memory_order_acquire)) {
            // QThread::start has no error return. A failed native thread create
            // must not leave its connection permanently unsettled.
            worker.state->cancel();
            { std::lock_guard lock(worker.state->mutex); worker.state->failure = AttemptReason::Network; }
            worker.state->finished.store(true, std::memory_order_release);
        }
    }
    std::erase_if(impl_->workers, [](const auto& worker) {
        // A finished snapshot alone is not proof that native thread teardown
        // (including TLS) completed. wait(0) is a zero-time join check.
        return worker.state->finished.load(std::memory_order_acquire) && worker.thread->wait(0);
    });
    if (impl_->workers.empty()) impl_->timer.stop();
}
ConnectionManager::ConnectionManager(ConnectionRuntime& runtime, QObject* parent)
    : QObject(parent), runtime_(&runtime) {
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    timer_.setInterval(10);
}
ConnectionManager::~ConnectionManager() {
    stopWorker();
    if (reserved_ && runtime_) runtime_->releaseReservation();
}
void ConnectionManager::owner() const {
    if (thread() != QThread::currentThread() || timer_.thread() != QThread::currentThread())
        throw std::logic_error("Connection manager owner violation");
}
ConnectionStatus ConnectionManager::status() const {
    owner();
    if (!attempt_) return {};
    return {attempt_->id(), attempt_->phase(), attempt_->reason(),
        terminal(attempt_->phase()) && !resolver_.busy() && (!state_ || state_->finished.load(std::memory_order_acquire)),
        state_ ? state_->output.usage().bytes : 0};
}
bool ConnectionManager::start(const Endpoint& endpoint, const std::optional<TrustedHostPin>& saved,
                              AttemptTimeouts timeouts, int columns, int rows, std::shared_ptr<SessionWork> work, std::optional<std::uint16_t> loopbackTunnel) {
    owner();
    const auto previous = status();
    if (!runtime_ || runtime_->stopping() || runtime_->workers() >= ConnectionRuntime::MaxWorkers ||
        (attempt_ && (!previous.settled || !terminal(previous.phase))) || !dimensionsValid(columns, rows) ||
        endpoint.user.empty() || endpoint.user.size() > 128 || !endpoint.port || (loopbackTunnel && !*loopbackTunnel)) return false;
    const auto host = HostResolver::canonicalHost(endpoint.host);
    if (!host || (saved && (saved->host.size() > 253 || saved->algorithm.empty() || saved->algorithm.size() > 128))) return false;
    Endpoint canonical{*host, endpoint.port, endpoint.user};
    std::unique_ptr<ConnectionAttempt> attempt;
    try { attempt = std::make_unique<ConnectionAttempt>(canonical, timeouts); }
    catch (const std::exception&) { return false; }
    if (!runtime_->reserve()) return false;
    reserved_ = true;
    endpoint_ = std::move(canonical); saved_ = saved; timeouts_ = timeouts;
    columns_ = columns; rows_ = rows;
    work_ = std::move(work);
    loopbackTunnel_ = loopbackTunnel;
    attempt_ = std::move(attempt); state_.reset();
    seenHost_ = seenAuth_ = approvalSent_ = authSent_ = false;
    const auto id = attempt_->id();
    const auto started = resolver_.start(id, loopbackTunnel_ ? "127.0.0.1" : endpoint_.host, attempt_->deadline(), [this, id](Resolution result) {
        if (!attempt_ || attempt_->id() != id || attempt_->phase() != AttemptPhase::Connecting) return;
        attempt_->tick();
        if (attempt_->stopToken().stop_requested()) return;
        if (result.status != ResolutionStatus::Success) {
            if (reserved_ && runtime_) runtime_->releaseReservation();
            reserved_ = false;
            attempt_->failed(id, result.status == ResolutionStatus::Timeout ? AttemptReason::Timeout : AttemptReason::Network);
            return;
        }
        state_ = std::make_shared<detail::ConnectionState>();
        state_->endpoint = endpoint_; state_->addresses = std::move(result.addresses);
        state_->request = id; state_->keepAlive = keepAlive_;
        state_->work = work_;
        state_->loopbackTunnel = loopbackTunnel_;
        state_->connectDeadline = attempt_->deadline(); state_->timeouts = timeouts_;
        state_->dimensions = {columns_, rows_};
        const bool launched = runtime_ && runtime_->launch(state_);
        reserved_ = false;
        if (!launched) {
            state_->cancel(); state_->finished = true;
            attempt_->failed(id, AttemptReason::Network);
        }
    });
    if (!started) {
        if (reserved_ && runtime_) runtime_->releaseReservation();
        reserved_ = false;
        attempt_->failed(id, AttemptReason::Network); return false;
    }
    timer_.start();
    return true;
}
void ConnectionManager::stopWorker() { if (state_) state_->cancel(); }
void ConnectionManager::poll() {
    if (!attempt_) return;
    if (!runtime_ || runtime_->stopping()) cancel();
    attempt_->tick();
    if (attempt_->stopToken().stop_requested()) {
        if (resolver_.busy()) resolver_.cancel(attempt_->id());
        if (reserved_ && runtime_) runtime_->releaseReservation();
        reserved_ = false;
        stopWorker();
    }
    if (state_) {
        std::optional<HostIdentity> host;
        bool authenticated;
        AttemptReason failure;
        const bool finished = state_->finished.load(std::memory_order_acquire);
        {
            std::lock_guard lock(state_->mutex);
            if (!seenHost_) host = state_->host;
            authenticated = state_->authenticated;
            failure = state_->failure;
        }
        if (host && !seenHost_) {
            seenHost_ = true;
            try { attempt_->connected(attempt_->id(), *host, saved_); }
            catch (const std::exception&) { attempt_->failed(attempt_->id(), AttemptReason::Protocol); }
            if (auto approval = attempt_->takeCachedApproval(attempt_->id())) deliverApproval(*approval);
        }
        if (authenticated && !seenAuth_) {
            seenAuth_ = true;
            attempt_->authenticated(attempt_->id());
        }
        if (attempt_->stopToken().stop_requested()) stopWorker();
        if (finished) {
            if (failure == AttemptReason::None) attempt_->workerClosed(attempt_->id());
            else attempt_->failed(attempt_->id(), failure);
        }
    } else if (attempt_->phase() == AttemptPhase::Closing && !resolver_.busy()) {
        attempt_->workerClosed(attempt_->id());
    }
    if (status().settled && terminal(attempt_->phase())) timer_.stop();
}
std::optional<HostPrompt> ConnectionManager::hostPrompt() { owner(); poll(); return attempt_ ? attempt_->prompt() : std::nullopt; }
void ConnectionManager::deliverApproval(const HostIdentity& identity) {
    if (!state_ || approvalSent_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->approval = identity;
        state_->authenticationDeadline = attempt_->deadline();
    }
    approvalSent_ = true;
    state_->wake.notify_all();
}
bool ConnectionManager::approve(AttemptId request, AttemptId challenge) {
    owner();
    poll();
    if (!attempt_) return false;
    auto approval = attempt_->approve(request, challenge);
    if (!approval) { if (attempt_->stopToken().stop_requested()) stopWorker(); return false; }
    deliverApproval(*approval);
    return approvalSent_;
}
bool ConnectionManager::reject(AttemptId request) {
    owner();
    if (!attempt_ || !attempt_->reject(request)) return false;
    stopWorker(); return true;
}
bool ConnectionManager::authenticate(AttemptId request, Credentials credentials) {
    owner();
    poll();
    if (!attempt_) return false;
    attempt_->tick();
    if (request != attempt_->id() || attempt_->phase() != AttemptPhase::Authentication ||
        !approvalSent_ || authSent_ || !state_ || state_->finished || !credentials.valid()) return false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stop.stop_requested() || state_->credentials) return false;
        state_->credentials = std::move(credentials);
    }
    authSent_ = true;
    state_->wake.notify_all();
    return true;
}
bool ConnectionManager::cancel() {
    owner();
    if (!attempt_) return false;
    const bool changed = attempt_->cancel();
    if (resolver_.busy()) resolver_.cancel(attempt_->id());
    if (reserved_ && runtime_) runtime_->releaseReservation();
    reserved_ = false;
    stopWorker();
    return changed;
}
std::optional<AuthenticationPrompt> ConnectionManager::authenticationPrompt() {
    owner(); poll();
    if (!state_ || !attempt_ || attempt_->phase() != AttemptPhase::Authentication) return {};
    std::lock_guard lock(state_->mutex);
    if (state_->answers) return {};
    return state_->interactive;
}
bool ConnectionManager::answerAuthentication(AttemptId request, AttemptId challenge, Session::InteractiveAnswers answers) {
    owner(); poll();
    if (!state_ || !attempt_ || request != attempt_->id() || attempt_->phase() != AttemptPhase::Authentication) return false;
    std::lock_guard lock(state_->mutex);
    if (!state_->interactive || state_->interactive->challenge != challenge || state_->answers ||
        Clock::now() >= state_->interactive->deadline || state_->stop.stop_requested() ||
        answers.size() != state_->interactive->content.questions.size()) return false;
    for (const auto& answer : answers) if (answer.bytes().size() > 4096 ||
        std::ranges::find(answer.bytes(), std::byte{0}) != answer.bytes().end()) return false;
    state_->answers = std::move(answers); state_->wake.notify_all(); return true;
}
bool ConnectionManager::setKeepAlive(std::chrono::seconds interval) {
    owner();
    if (interval.count() < 0 || interval > std::chrono::hours(1)) return false;
    keepAlive_ = interval;
    if (state_) { std::lock_guard lock(state_->mutex); state_->keepAlive = interval; }
    return true;
}
bool ConnectionManager::send(std::span<const std::byte> bytes) {
    owner();
    poll(); // Observe a just-finished peer before accepting more terminal input.
    if (work_ || !attempt_ || attempt_->phase() != AttemptPhase::Active || !state_ || !state_->input.tryPush(bytes)) return false;
    state_->wake.notify_all(); return true;
}
bool ConnectionManager::resize(int columns, int rows) {
    owner();
    if (work_) return false;
    if (!dimensionsValid(columns, rows) || !attempt_ || terminal(attempt_->phase()) || attempt_->phase() == AttemptPhase::Closing) return false;
    columns_ = columns; rows_ = rows;
    if (state_) {
        { std::lock_guard lock(state_->mutex); state_->dimensions = {columns, rows}; }
        state_->wake.notify_all();
    }
    return true;
}
std::optional<vault::SecretBytes> ConnectionManager::takeOutput() {
    owner();
    if (!state_) return {};
    if (attempt_ && !terminal(attempt_->phase()) && attempt_->phase() != AttemptPhase::Active &&
        attempt_->phase() != AttemptPhase::Closing) return {};
    auto chunk = state_->output.tryPop();
    if (chunk) state_->wake.notify_all();
    return chunk;
}
} // namespace nordssh::ssh
