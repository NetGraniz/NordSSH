#pragma once
#include "VaultFormat.h"
#include <span>
#include <stdexcept>
#include <vector>

namespace nordssh::vault {
class CryptoError final : public std::runtime_error {
public:
    CryptoError() : std::runtime_error("Vault authentication or cryptographic operation failed") {}
};

// Move-only, zero-initialized allocation; explicitly cleansed on every exit.
// Not locked memory, and not protection against same-user malware or crash dumps.
class SecretBytes final {
public:
    explicit SecretBytes(std::size_t size);
    ~SecretBytes();
    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;
    SecretBytes(SecretBytes&& other) noexcept;
    SecretBytes& operator=(SecretBytes&& other) noexcept;
    [[nodiscard]] std::span<std::byte> bytes() noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }
private:
    std::byte* data_{};
    std::size_t size_{};
};

using KeyView = std::span<const std::byte, 32>;
using NonceView = std::span<const std::byte, 12>;
using Tag = std::array<std::byte, 16>;
struct Ciphertext { std::vector<std::byte> bytes; Tag tag; };

[[nodiscard]] Ciphertext encryptGcm(KeyView key, NonceView nonce,
    std::span<const std::byte> plaintext, std::span<const std::byte> aad);
[[nodiscard]] SecretBytes decryptGcm(KeyView key, NonceView nonce,
    std::span<const std::byte> ciphertext, std::span<const std::byte> aad, const Tag& tag);
// Blocking KDF: worker thread only. At most two derivations process-wide.
// Caller owns/wipes the password; this function never retains it.
[[nodiscard]] SecretBytes derivePasswordKey(std::string_view password,
    std::span<const std::byte, 16> salt);

struct OpenedEnvelope {
    Header header;
    SecretBytes key;
    SecretBytes payload;
};
// In-memory authenticated envelope only. These APIs neither parse JSON nor write
// files. The future VaultService MUST validate the strict JSON/record schema
// before publishing an unlocked model or committing any encrypted candidate.
[[nodiscard]] std::vector<std::byte> createEnvelope(std::string_view password,
    std::span<const std::byte> validatedPayload);
[[nodiscard]] OpenedEnvelope openEnvelope(std::span<const std::byte> file, std::string_view password);
[[nodiscard]] OpenedEnvelope openEnvelopeWithKey(std::span<const std::byte> file, KeyView key);
[[nodiscard]] std::vector<std::byte> resealEnvelope(const OpenedEnvelope& opened,
    std::span<const std::byte> validatedPayload);
} // namespace nordssh::vault
