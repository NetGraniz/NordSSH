#pragma once
#include "PrivateFiles.h"
#include "VaultDocument.h"
#include <atomic>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

namespace nordssh::vault {
class ServiceError final : public std::runtime_error {
public: ServiceError() : std::runtime_error("Vault operation refused: locked, busy, cancelled or externally changed") {}
};
class VaultLease final {
public:
    VaultLease() = default;
    ~VaultLease();
    VaultLease(VaultLease&&) noexcept;
    VaultLease& operator=(VaultLease&&) noexcept;
    VaultLease(const VaultLease&) = delete;
    VaultLease& operator=(const VaultLease&) = delete;
private:
    friend class VaultService;
    explicit VaultLease(std::shared_ptr<std::atomic<unsigned>> count) : count_(std::move(count)) {}
    std::shared_ptr<std::atomic<unsigned>> count_;
};
// Worker-thread owner, never GUI-thread I/O/KDF. Returned model references are
// owner-thread only. A lease prevents lock/switch/import while a session uses data.
class VaultService final {
public:
    explicit VaultService(QString privateDirectory);
    [[nodiscard]] QString create(std::string_view password, std::stop_token stop = {});
    void unlock(const QString& file, std::string_view password, std::stop_token stop = {});
    void lock();
    void rememberDevice();
    [[nodiscard]] bool unlockRemembered(const QString& file, std::stop_token stop = {});
    void forgetDevice();
    [[nodiscard]] bool unlocked() const;
    [[nodiscard]] const VaultDocument& document() const;
    [[nodiscard]] VaultLease lease();
    void save(const VaultDocument&, std::stop_token stop = {}, const std::function<void()>& commitGate = {});
    [[nodiscard]] QString exportEncrypted(std::string_view exportPassword, std::stop_token stop = {});
    // Explicit full replacement only, not an implicit merge. Current vault password
    // and key remain unchanged. Source is authenticated and fully admitted first.
    void importEncrypted(std::span<const std::byte> source, std::string_view sourcePassword,
                         bool confirmedReplace, std::stop_token stop = {});
    [[nodiscard]] const PrivateFiles& files() const { owner(); return files_; }
private:
    void owner() const;
    void idle() const;
    void active() const;
    void unchanged() const;
    void replace(const VaultDocument&, std::stop_token, const std::function<void()>&);
    PrivateFiles files_;
    const std::thread::id owner_ = std::this_thread::get_id();
    QString current_;
    std::vector<std::byte> snapshot_;
    std::optional<OpenedEnvelope> envelope_;
    std::optional<VaultDocument> document_;
    std::shared_ptr<std::atomic<unsigned>> leases_ = std::make_shared<std::atomic<unsigned>>(0);
};
} // namespace nordssh::vault
