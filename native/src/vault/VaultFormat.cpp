#include "VaultFormat.h"
#include <algorithm>
#include <limits>

namespace nordssh::vault {
PasswordStatus validatePassword(std::string_view utf8) noexcept {
    if (utf8.size() > MaxPasswordBytes) return PasswordStatus::TooLong;
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[offset++]);
        std::uint32_t scalar = lead;
        int continuation = 0;
        std::uint32_t minimum = 0;
        if (lead <= 0x7f) {}
        else if (lead >= 0xc2 && lead <= 0xdf) {
            scalar = lead & 0x1f; continuation = 1; minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            scalar = lead & 0x0f; continuation = 2; minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            scalar = lead & 0x07; continuation = 3; minimum = 0x10000;
        } else return PasswordStatus::InvalidUtf8;
        if (utf8.size() - offset < static_cast<std::size_t>(continuation)) return PasswordStatus::InvalidUtf8;
        for (int index = 0; index < continuation; ++index) {
            const auto next = static_cast<unsigned char>(utf8[offset++]);
            if ((next & 0xc0) != 0x80) return PasswordStatus::InvalidUtf8;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            return PasswordStatus::InvalidUtf8;
        ++count;
    }
    return count < 7 ? PasswordStatus::TooShort : PasswordStatus::Valid;
}
namespace {
std::uint64_t littleEndian(std::span<const std::byte> bytes, std::size_t start, std::size_t count) noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < count; ++index)
        result |= std::uint64_t(std::to_integer<unsigned char>(bytes[start + index])) << (index * 8);
    return result;
}
bool zero(std::span<const std::byte> bytes, std::size_t start, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index)
        if (bytes[start + index] != std::byte{}) return false;
    return true;
}
} // namespace
HeaderCheck validateHeader(std::span<const std::byte> header, std::uint64_t fileSize) noexcept {
    if (header.size() != HeaderSize || fileSize < HeaderSize + TagSize + 1 || fileSize > MaxFileBytes)
        return {HeaderStatus::Size};
    constexpr std::string_view magic{"NORDVLT\0", 8};
    for (std::size_t index = 0; index < magic.size(); ++index)
        if (std::to_integer<unsigned char>(header[index]) != static_cast<unsigned char>(magic[index]))
            return {HeaderStatus::Magic};
    if (littleEndian(header, 8, 4) != 1) return {HeaderStatus::Version};
    if (littleEndian(header, 12, 4) != HeaderSize) return {HeaderStatus::HeaderLength};
    if (!zero(header, 16, 4)) return {HeaderStatus::Flags};
    if (!zero(header, 20, 4) || !zero(header, 156, 4)) return {HeaderStatus::Reserved};
    if ((std::to_integer<unsigned char>(header[30]) & 0xf0) != 0x40 ||
        (std::to_integer<unsigned char>(header[32]) & 0xc0) != 0x80) return {HeaderStatus::Uuid};
    if (littleEndian(header, 40, 4) != 1) return {HeaderStatus::KdfProfile};
    const auto length = littleEndian(header, 132, 8);
    if (length < 1 || length > MaxPlaintextBytes || fileSize != HeaderSize + length + TagSize)
        return {HeaderStatus::PayloadLength};
    const auto revision = littleEndian(header, 140, 8);
    if (revision < 1 || revision > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        return {HeaderStatus::Revision};
    const auto keyRevision = littleEndian(header, 148, 8);
    if (keyRevision < 1 || keyRevision > revision || revision - keyRevision >= KeyRevisionLimit)
        return {HeaderStatus::KeyRevision};
    return {HeaderStatus::Valid, length, revision, keyRevision};
}
std::array<std::byte, 83> wrapAad(const Header& header) noexcept {
    std::array<std::byte, 83> result{};
    for (std::size_t index = 0; index < WrapDomain.size(); ++index)
        result[index] = std::byte(static_cast<unsigned char>(WrapDomain[index]));
    std::copy_n(header.begin(), 60, result.begin() + WrapDomain.size());
    return result;
}
std::array<std::byte, 183> dataAad(const Header& header) noexcept {
    std::array<std::byte, 183> result{};
    for (std::size_t index = 0; index < DataDomain.size(); ++index)
        result[index] = std::byte(static_cast<unsigned char>(DataDomain[index]));
    std::copy(header.begin(), header.end(), result.begin() + DataDomain.size());
    return result;
}
} // namespace nordssh::vault
