#include "HostResolver.h"
#include <QHostAddress>
#include <QThread>
#include <algorithm>
#include <stdexcept>

namespace nordssh::ssh {
namespace {
class QtResolver final : public ResolverBackend {
public:
    int start(const QString& host, QObject* context, std::function<void(QHostInfo)> done) override {
        return QHostInfo::lookupHost(host, context, std::move(done));
    }
    void abort(int lookup) override { QHostInfo::abortHostLookup(lookup); }
};
}
HostResolver::HostResolver(QObject* parent) : HostResolver(std::make_unique<QtResolver>(), parent) {}
HostResolver::HostResolver(std::unique_ptr<ResolverBackend> backend, QObject* parent)
    : QObject(parent), backend_(std::move(backend)) {
    if (!backend_) throw std::invalid_argument("Missing resolver backend");
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { expire(); });
}
HostResolver::~HostResolver() {
    timer_.stop();
    if (active_ && active_->lookup >= 0) backend_->abort(active_->lookup);
    active_.reset(); // No owner callback during destruction.
}
void HostResolver::owner() const {
    if (thread() != QThread::currentThread() || timer_.thread() != QThread::currentThread())
        throw std::logic_error("Resolver used outside its event-loop owner");
}
bool HostResolver::busy() const { owner(); return static_cast<bool>(active_); }
std::optional<std::string> HostResolver::canonicalHost(std::string_view host) {
    if (host.empty() || host.size() > 253 || std::ranges::any_of(host, [](unsigned char c) {
        return c <= 0x20 || c >= 0x7f || c == '%';
    })) return {};
    QHostAddress literal;
    if (literal.setAddress(QString::fromLatin1(host.data(), static_cast<qsizetype>(host.size()))))
        return literal.toString().toStdString();
    if (host.back() == '.') host.remove_suffix(1);
    if (host.empty() || std::ranges::all_of(host, [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) return {};
    std::string canonical;
    canonical.reserve(host.size());
    std::size_t label = 0;
    for (const char c : host) {
        if (c == '.') {
            if (!label || canonical.back() == '-') return {};
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return {};
            if ((!label && c == '-') || ++label > 63) return {};
        }
        canonical += c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    }
    if (!label || canonical.back() == '-') return {};
    return canonical;
}
bool HostResolver::start(ResolutionId request, std::string_view host, Clock::time_point deadline, Completion done) {
    owner();
    if (active_ || !done) return false;
    const auto canonical = canonicalHost(host);
    const auto now = Clock::now();
    active_ = std::make_shared<Ticket>(Ticket{request, canonical.value_or(""), deadline, std::move(done), -1,
        canonical.has_value() && (deadline <= now || deadline - now <= std::chrono::seconds(30))});
    const auto ticket = active_;
    // Always deliver asynchronously, including literals and admission failures.
    QTimer::singleShot(0, this, [this, ticket] { begin(ticket); });
    return true;
}
void HostResolver::begin(const std::shared_ptr<Ticket>& ticket) {
    if (active_ != ticket) return;
    if (!ticket->valid) { complete(ticket, ResolutionStatus::Invalid); return; }
    const auto now = Clock::now();
    if (now >= ticket->deadline) { complete(ticket, ResolutionStatus::Timeout); return; }
    // No completing callback here: it could destroy this object before begin
    // has dispatched the lookup. Use the time already checked above.
    timer_.start(static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(ticket->deadline - now).count()));
    QHostAddress literal;
    if (literal.setAddress(QString::fromStdString(ticket->host))) {
        complete(ticket, ResolutionStatus::Success, {ticket->host});
        return;
    }
    ticket->lookup = backend_->start(QString::fromStdString(ticket->host), this, [this, ticket](const QHostInfo& info) {
        if (active_ != ticket) return;
        ticket->lookup = -1;
        if (Clock::now() >= ticket->deadline) { complete(ticket, ResolutionStatus::Timeout); return; }
        std::vector<std::string> addresses;
        if (info.error() == QHostInfo::NoError) {
            for (const auto& address : info.addresses()) {
                if (!address.scopeId().isEmpty() ||
                    (address.protocol() != QAbstractSocket::IPv4Protocol && address.protocol() != QAbstractSocket::IPv6Protocol)) continue;
                const auto text = address.toString().toStdString();
                if (std::ranges::find(addresses, text) == addresses.end()) addresses.push_back(text);
                if (addresses.size() == 8) break;
            }
        }
        const auto status = addresses.empty() ? ResolutionStatus::NotFound : ResolutionStatus::Success;
        complete(ticket, status, std::move(addresses));
    });
    if (ticket->lookup < 0) complete(ticket, ResolutionStatus::NotFound);
}
void HostResolver::complete(const std::shared_ptr<Ticket>& ticket, ResolutionStatus status, std::vector<std::string> addresses) {
    if (active_ != ticket) return;
    timer_.stop();
    if (ticket->lookup >= 0) backend_->abort(ticket->lookup);
    auto done = std::move(ticket->done);
    Resolution result{ticket->request, status, std::move(ticket->host), std::move(addresses)};
    active_.reset();
    // Callback may start a new attempt or destroy this resolver. Touch nothing after it.
    done(std::move(result));
}
bool HostResolver::cancel(ResolutionId request) {
    owner();
    if (!active_ || active_->request != request) return false;
    const auto ticket = active_;
    complete(ticket, ResolutionStatus::Cancelled);
    return true;
}
void HostResolver::expire() {
    if (!active_) return;
    const auto ticket = active_;
    const auto now = Clock::now();
    if (now >= ticket->deadline) { complete(ticket, ResolutionStatus::Timeout); return; }
    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(ticket->deadline - now).count();
    timer_.start(static_cast<int>(std::min<std::int64_t>(milliseconds, 30000)));
}
} // namespace nordssh::ssh
