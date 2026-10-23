#include "vault/VaultFormat.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace nordssh::vault;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void put(Header& header, std::size_t at, std::uint64_t value, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) header[at + index] = std::byte((value >> (8 * index)) & 0xff);
}
static Header fixture() {
    Header result{};
    const std::string_view magic{"NORDVLT\0", 8};
    for (std::size_t i = 0; i < magic.size(); ++i) result[i] = std::byte(magic[i]);
    put(result, 8, 1, 4); put(result, 12, HeaderSize, 4);
    result[30] = std::byte{0x40}; result[32] = std::byte{0x80};
    put(result, 40, 1, 4); put(result, 132, 2, 8); put(result, 140, 1, 8); put(result, 148, 1, 8);
    return result;
}
int main() {
    try {
        require(validatePassword("123456") == PasswordStatus::TooShort, "Six scalars accepted");
        require(validatePassword("1234567") == PasswordStatus::Valid, "Seven ASCII rejected");
        require(validatePassword("\xd0\xb0\xd0\xb1\xd0\xb2\xd0\xb3\xd0\xb4\xd0\xb5\xd0\xb6") == PasswordStatus::Valid, "Seven Cyrillic rejected");
        require(validatePassword("123456\xf0\x9f\x98\x80") == PasswordStatus::Valid, "Astral scalar");
        require(validatePassword("12345\xf0\x9f\x98\x80") == PasswordStatus::TooShort, "UTF16 units counted as scalars");
        require(validatePassword("       ") == PasswordStatus::Valid, "Password was trimmed");
        require(validatePassword("12345e\xcc\x81") == PasswordStatus::Valid, "Combining mark is a scalar");
        require(validatePassword(std::string("123456\0", 7)) == PasswordStatus::Valid, "NUL truncated");
        require(validatePassword(std::string(4096, 'x')) == PasswordStatus::Valid, "Password limit boundary");
        require(validatePassword(std::string(4097, 'x')) == PasswordStatus::TooLong, "Password over limit");
        for (const auto* invalid : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\x80", "\xe2\x82"})
            require(validatePassword(std::string("1234567") + invalid) == PasswordStatus::InvalidUtf8, "Invalid UTF8 accepted");
        auto header = fixture();
        constexpr std::uint64_t fileSize = 178;
        require(validateHeader(header, fileSize).valid(), "Valid header rejected");
        require(!validateHeader(std::span(header).first(159), fileSize).valid(), "Truncated header");
        require(!validateHeader(header, fileSize + 1).valid(), "Trailing bytes");
        require(!validateHeader(header, MaxFileBytes + 1).valid(), "Oversized file");
        require(!validateHeader({}, 178).valid(), "Empty header");
        require(!validateHeader(header, 0).valid(), "Empty file");
        require(!validateHeader(header, 176).valid(), "Empty payload file");
        require(!validateHeader(header, 0xffffffffffffffffULL).valid(), "Unbounded file length");
        for (const auto offset : {0U, 8U, 12U, 16U, 20U, 30U, 32U, 40U, 132U, 148U, 156U}) {
            auto changed = header;
            changed[offset] ^= std::byte{0xff};
            require(!validateHeader(changed, fileSize).valid(), "Invalid structural field accepted");
        }
        auto boundary = header;
        put(boundary, 132, 1, 8);
        require(validateHeader(boundary, 177).valid(), "Minimum payload boundary");
        put(boundary, 132, 0, 8);
        require(!validateHeader(boundary, 177).valid(), "Zero payload accepted");
        put(boundary, 132, MaxPlaintextBytes, 8);
        require(validateHeader(boundary, MaxFileBytes).valid(), "Maximum payload boundary");
        put(boundary, 132, 0xffffffffffffffffULL, 8);
        require(!validateHeader(boundary, MaxFileBytes).valid(), "Overflowed length");
        boundary = header;
        put(boundary, 140, KeyRevisionLimit, 8);
        require(validateHeader(boundary, fileSize).valid(), "Last key revision rejected");
        put(boundary, 140, KeyRevisionLimit + 1, 8);
        require(!validateHeader(boundary, fileSize).valid(), "DEK rotation boundary accepted");
        boundary = header;
        put(boundary, 140, 0, 8);
        require(!validateHeader(boundary, fileSize).valid(), "Revision zero");
        put(boundary, 140, 0x7fffffffffffffffULL, 8);
        put(boundary, 148, 0x7fffffffffffffffULL, 8);
        require(validateHeader(boundary, fileSize).valid(), "Maximum revision with rotated DEK");
        put(boundary, 140, 0x8000000000000000ULL, 8);
        require(!validateHeader(boundary, fileSize).valid(), "Revision high bit");
        boundary = header;
        put(boundary, 148, 0, 8);
        require(!validateHeader(boundary, fileSize).valid(), "Key revision zero");
        put(boundary, 148, 2, 8);
        require(!validateHeader(boundary, fileSize).valid(), "Key revision after file revision");
        const auto wrap = wrapAad(header);
        const auto data = dataAad(header);
        require(wrap[22] == std::byte{} && data[22] == std::byte{}, "AAD domain NUL");
        constexpr unsigned char expectedWrap[] = {0x4e,0x6f,0x72,0x64,0x53,0x53,0x48,0x2d,0x6e,0x61,0x74,0x69,0x76,0x65,0x2d,0x76,0x31,0x2d,0x77,0x72,0x61,0x70,0};
        constexpr unsigned char expectedData[] = {0x4e,0x6f,0x72,0x64,0x53,0x53,0x48,0x2d,0x6e,0x61,0x74,0x69,0x76,0x65,0x2d,0x76,0x31,0x2d,0x64,0x61,0x74,0x61,0};
        for (std::size_t i = 0; i < 23; ++i) {
            require(wrap[i] == std::byte(expectedWrap[i]), "Exact wrap domain");
            require(data[i] == std::byte(expectedData[i]), "Exact data domain");
        }
        require(std::equal(header.begin(), header.begin() + 60, wrap.begin() + 23), "Wrap AAD exact bytes");
        require(std::equal(header.begin(), header.end(), data.begin() + 23), "Data AAD exact bytes");
        header[156] = std::byte{1};
        require(wrapAad(header) == wrap && dataAad(header) != data, "Full header authentication scope");
        std::cout << "Vault format admission tests passed (not cryptographic authentication)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
