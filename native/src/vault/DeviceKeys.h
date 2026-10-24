#pragma once
#include "PrivateFiles.h"
#include "VaultCrypto.h"
#include <optional>

namespace nordssh::vault {
class DeviceError final : public std::runtime_error {
public: DeviceError() : std::runtime_error("Device key service unavailable or record invalid; use the vault password") {}
};
// Optional OS protection for a 32-byte DEK, never the portable encryption format.
// Identity binds canonical path, vault UUID and key revision. Worker-thread only.
class DeviceKeys final {
public:
    explicit DeviceKeys(const PrivateFiles& files) : files_(files) {}
    void remember(const QString& identity, KeyView key) const;
    [[nodiscard]] std::optional<SecretBytes> lookup(const QString& identity) const;
    void forget(const QString& identity) const;
private:
    const PrivateFiles& files_;
};
[[nodiscard]] QString deviceIdentity(const QString& canonicalPath, const Header& header);
} // namespace nordssh::vault
