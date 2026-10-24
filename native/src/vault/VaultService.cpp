#include "VaultService.h"
#include "DeviceKeys.h"
#include <QCoreApplication>
#include <QLockFile>
#include <QThread>
#include <openssl/rand.h>

namespace nordssh::vault {
namespace {
void checkpoint(std::stop_token stop) { if (stop.stop_requested()) throw ServiceError(); }
QString workerDirectory(QString directory) {
    if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()) throw ServiceError();
    return directory;
}
QString generated(const char* extension) {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw CryptoError();
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())).toHex()) + QLatin1StringView(extension);
}
void acquire(QLockFile& lock) {
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) throw ServiceError();
}
}
VaultLease::~VaultLease() { if (count_) count_->fetch_sub(1); }
VaultLease::VaultLease(VaultLease&& other) noexcept : count_(std::move(other.count_)) {}
VaultLease& VaultLease::operator=(VaultLease&& other) noexcept {
    if (this != &other) { if (count_) count_->fetch_sub(1); count_ = std::move(other.count_); }
    return *this;
}
VaultService::VaultService(QString directory) : files_(workerDirectory(std::move(directory))) { owner(); }
void VaultService::owner() const {
    if (std::this_thread::get_id() != owner_ || (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread())) throw ServiceError();
}
void VaultService::idle() const { owner(); if (leases_->load() != 0) throw ServiceError(); }
void VaultService::active() const { owner(); if (!envelope_ || !document_) throw ServiceError(); }
bool VaultService::unlocked() const { owner(); return envelope_.has_value(); }
const VaultDocument& VaultService::document() const { active(); return *document_; }
VaultLease VaultService::lease() {
    active(); if (leases_->load() >= 64) throw ServiceError();
    leases_->fetch_add(1); return VaultLease(leases_);
}
void VaultService::unchanged() const {
    if (files_.read(current_) != snapshot_) throw ServiceError();
}
QString VaultService::create(std::string_view password, std::stop_token stop) {
    idle(); checkpoint(stop);
    auto plain = serializeDocument(VaultDocument{});
    auto bytes = createEnvelope(password, plain.bytes());
    auto opened = openEnvelope(bytes, password);
    auto model = parseDocument(opened.payload.bytes());
    const auto file = generated(".nordvault");
    QLockFile lock(files_.path(QStringLiteral("store.lock"))); acquire(lock);
    if (files_.exists(file)) throw ServiceError();
    files_.write(file, bytes, [&] { checkpoint(stop); });
    current_ = file; snapshot_ = std::move(bytes); envelope_ = std::move(opened); document_ = std::move(model);
    return file;
}
void VaultService::unlock(const QString& file, std::string_view password, std::stop_token stop) {
    idle(); checkpoint(stop);
    if (!file.endsWith(QStringLiteral(".nordvault"))) throw ServiceError();
    QLockFile lock(files_.path(QStringLiteral("store.lock"))); acquire(lock);
    auto bytes = files_.read(file);
    auto opened = openEnvelope(bytes, password);
    auto model = parseDocument(opened.payload.bytes()); checkpoint(stop);
    // Publish only after all admission succeeds; failed switch preserves old model.
    current_ = file; snapshot_ = std::move(bytes); envelope_ = std::move(opened); document_ = std::move(model);
}
void VaultService::lock() { idle(); document_.reset(); envelope_.reset(); snapshot_.clear(); current_.clear(); }
void VaultService::rememberDevice() {
    active(); QLockFile lock(files_.path(QStringLiteral("store.lock"))); acquire(lock); unchanged();
    DeviceKeys(files_).remember(deviceIdentity(files_.path(current_), envelope_->header), KeyView(envelope_->key.bytes().data(), 32));
}
bool VaultService::unlockRemembered(const QString& file, std::stop_token stop) {
    idle(); checkpoint(stop);
    if (!file.endsWith(QStringLiteral(".nordvault"))) throw ServiceError();
    QLockFile lock(files_.path(QStringLiteral("store.lock"))); acquire(lock);
    auto bytes = files_.read(file);
    if (bytes.size() < HeaderSize || !validateHeader(std::span(bytes).first(HeaderSize), bytes.size()).valid()) throw CryptoError();
    Header header{}; std::copy_n(bytes.begin(), HeaderSize, header.begin());
    auto key = DeviceKeys(files_).lookup(deviceIdentity(files_.path(file), header));
    if (!key) return false;
    auto opened = openEnvelopeWithKey(bytes, KeyView(key->bytes().data(), 32));
    auto model = parseDocument(opened.payload.bytes()); checkpoint(stop);
    current_ = file; snapshot_ = std::move(bytes); envelope_ = std::move(opened); document_ = std::move(model); return true;
}
void VaultService::forgetDevice() {
    active(); DeviceKeys(files_).forget(deviceIdentity(files_.path(current_), envelope_->header));
}
void VaultService::replace(const VaultDocument& model, std::stop_token stop, const std::function<void()>& gate) {
    active(); checkpoint(stop);
    auto plain = serializeDocument(model);
    auto candidate = resealEnvelope(*envelope_, plain.bytes());
    auto opened = openEnvelopeWithKey(candidate, KeyView(envelope_->key.bytes().data(), 32));
    auto checked = parseDocument(opened.payload.bytes());
    QLockFile lock(files_.path(QStringLiteral("store.lock"))); acquire(lock);
    unchanged(); checkpoint(stop);
    // One rolling encrypted predecessor per vault, not unlimited copies per edit.
    const auto backup = current_ + QStringLiteral(".previous");
    files_.write(backup, snapshot_, [&] { checkpoint(stop); });
    files_.write(current_, candidate, [&] { if (gate) gate(); checkpoint(stop); unchanged(); });
    snapshot_ = std::move(candidate); envelope_ = std::move(opened); document_ = std::move(checked);
}
void VaultService::save(const VaultDocument& model, std::stop_token stop, const std::function<void()>& gate) { replace(model, stop, gate); }
QString VaultService::exportEncrypted(std::string_view password, std::stop_token stop) {
    active(); checkpoint(stop);
    auto plain = serializeDocument(*document_);
    auto bytes = createEnvelope(password, plain.bytes());
    const auto file = generated(".export");
    QLockFile lock(files_.path(QStringLiteral("store.lock"))); acquire(lock);
    if (files_.exists(file)) throw ServiceError();
    files_.write(file, bytes, [&] { checkpoint(stop); }); return file;
}
void VaultService::importEncrypted(std::span<const std::byte> source, std::string_view password, bool confirmed, std::stop_token stop) {
    active(); idle(); if (!confirmed) throw ServiceError(); checkpoint(stop);
    auto opened = openEnvelope(source, password);
    auto model = parseDocument(opened.payload.bytes()); checkpoint(stop);
    replace(model, stop, {});
}
} // namespace nordssh::vault
