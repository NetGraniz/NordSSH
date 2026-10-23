#pragma once
#include <string_view>
#include <array>
#include <cstddef>

namespace nordssh::ssh {
// Structural cost admission only, not cryptographic key verification. Initial
// scope: one OpenSSH key, none or aes256-ctr/bcrypt (1..64 rounds, 16..64-byte
// salt), <=16 KiB private section. RSA is 2048..8192 bits with a <=32-bit
// exponent; ECDSA P-256/384/521 and Ed25519 use fixed public shapes. The returned
// SHA256 binds the admitted public section to the actual imported private key.
// Other formats/ciphers require separately reviewed import policies.
// Throws SessionError before calling a private-key decoder/KDF on bad input.
[[nodiscard]] std::array<std::byte,32> admitOpenSshKey(std::string_view privateKey);
}
