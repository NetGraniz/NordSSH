#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nordssh::vault {
inline constexpr std::size_t HeaderSize = 160;
inline constexpr std::size_t TagSize = 16;
inline constexpr std::size_t MaxPlaintextBytes = 8 * 1024 * 1024;
inline constexpr std::size_t MaxFileBytes = HeaderSize + MaxPlaintextBytes + TagSize;
inline constexpr std::size_t MaxPasswordBytes = 4096;
inline constexpr std::uint64_t KeyRevisionLimit = 1048576;
inline constexpr std::string_view WrapDomain{"NordSSH-native-v1-wrap\0", 23};
inline constexpr std::string_view DataDomain{"NordSSH-native-v1-data\0", 23};
using Header = std::array<std::byte, HeaderSize>;
enum class PasswordStatus { Valid, TooShort, TooLong, InvalidUtf8 };
[[nodiscard]] PasswordStatus validatePassword(std::string_view utf8) noexcept;

// Structural admission only, BEFORE KDF/allocation. This does not authenticate
// or decrypt a vault. Never treat the result as an unlocked data model.
enum class HeaderStatus {
    Valid, Size, Magic, Version, HeaderLength, Flags, Reserved, Uuid,
    KdfProfile, PayloadLength, Revision, KeyRevision
};
struct HeaderCheck {
    HeaderStatus status;
    std::uint64_t payloadLength{};
    std::uint64_t revision{};
    std::uint64_t keyRevision{};
    [[nodiscard]] bool valid() const noexcept { return status == HeaderStatus::Valid; }
};
[[nodiscard]] HeaderCheck validateHeader(std::span<const std::byte> header, std::uint64_t fileSize) noexcept;
[[nodiscard]] std::array<std::byte, 83> wrapAad(const Header& header) noexcept;
[[nodiscard]] std::array<std::byte, 183> dataAad(const Header& header) noexcept;
} // namespace nordssh::vault
