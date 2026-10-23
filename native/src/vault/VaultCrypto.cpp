#include "VaultCrypto.h"
#include <argon2.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <memory>
#include <semaphore>
#include <utility>

namespace nordssh::vault {
namespace {
using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
const unsigned char* raw(std::span<const std::byte> bytes) {
    return reinterpret_cast<const unsigned char*>(bytes.data());
}
unsigned char* raw(std::span<std::byte> bytes) {
    return reinterpret_cast<unsigned char*>(bytes.data());
}
void checked(int result) { if (result != 1) throw CryptoError(); }
void checkSizes(std::size_t bytes, std::size_t aad) {
    if (bytes > MaxPlaintextBytes || aad > 4096) throw CryptoError();
}
void writeLe(Header& header, std::size_t at, std::uint64_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) header[at+i] = std::byte((value >> (8*i)) & 0xff);
}
void random(std::span<std::byte> bytes, bool secret = false) {
    if (bytes.empty() || bytes.size() > HeaderSize) throw CryptoError();
    const auto size = static_cast<int>(bytes.size());
    checked(secret ? RAND_priv_bytes(raw(bytes), size) : RAND_bytes(raw(bytes), size));
}
Header admittedHeader(std::span<const std::byte> file) {
    if (file.size() < HeaderSize || file.size() > MaxFileBytes) throw CryptoError();
    Header header{};
    std::copy_n(file.begin(), HeaderSize, header.begin());
    if (!validateHeader(header, file.size()).valid()) throw CryptoError();
    return header;
}
KeyView asKey(const SecretBytes& bytes) {
    if (bytes.bytes().size() != 32) throw CryptoError();
    return KeyView(bytes.bytes().data(), 32);
}
NonceView nonceAt(const Header& header, std::size_t offset) {
    return NonceView(header.data() + offset, 12);
}
std::vector<std::byte> seal(Header header, KeyView key, std::span<const std::byte> payload) {
    if (payload.empty() || payload.size() > MaxPlaintextBytes) throw CryptoError();
    random(std::span(header).subspan(120, 12));
    writeLe(header, 132, payload.size(), 8);
    const auto aad = dataAad(header);
    const auto encrypted = encryptGcm(key, nonceAt(header, 120), payload, aad);
    std::vector<std::byte> file;
    file.reserve(HeaderSize + payload.size() + TagSize);
    file.insert(file.end(), header.begin(), header.end());
    file.insert(file.end(), encrypted.bytes.begin(), encrypted.bytes.end());
    file.insert(file.end(), encrypted.tag.begin(), encrypted.tag.end());
    return file;
}
} // namespace

SecretBytes::SecretBytes(std::size_t size) : size_(size) {
    if (size > MaxPlaintextBytes + 16) throw CryptoError();
    if (size == 0) return;
    data_ = static_cast<std::byte*>(OPENSSL_zalloc(size));
    if (!data_) throw std::bad_alloc();
}
SecretBytes::~SecretBytes() { OPENSSL_clear_free(data_, size_); }
SecretBytes::SecretBytes(SecretBytes&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}
SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept {
    if (this != &other) {
        OPENSSL_clear_free(data_, size_);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

Ciphertext encryptGcm(KeyView key, NonceView nonce,
    std::span<const std::byte> plaintext, std::span<const std::byte> aad) {
    checkSizes(plaintext.size(), aad.size());
    Context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!context) throw CryptoError();
    checked(EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr));
    checked(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr));
    checked(EVP_EncryptInit_ex(context.get(), nullptr, nullptr, raw(key), raw(nonce)));
    int length = 0;
    if (!aad.empty()) checked(EVP_EncryptUpdate(context.get(), nullptr, &length, raw(aad), static_cast<int>(aad.size())));
    Ciphertext result{std::vector<std::byte>(plaintext.size() + 16), {}};
    int written = 0;
    checked(EVP_EncryptUpdate(context.get(), raw(std::span(result.bytes)), &written,
                             raw(plaintext), static_cast<int>(plaintext.size())));
    if (written < 0 || static_cast<std::size_t>(written) > plaintext.size()) throw CryptoError();
    int finalLength = 0;
    checked(EVP_EncryptFinal_ex(context.get(), raw(std::span(result.bytes).subspan(static_cast<std::size_t>(written))), &finalLength));
    if (finalLength != 0 || static_cast<std::size_t>(written) != plaintext.size()) throw CryptoError();
    result.bytes.resize(plaintext.size());
    checked(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, 16, result.tag.data()));
    return result;
}

SecretBytes decryptGcm(KeyView key, NonceView nonce,
    std::span<const std::byte> ciphertext, std::span<const std::byte> aad, const Tag& tag) {
    checkSizes(ciphertext.size(), aad.size());
    Context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!context) throw CryptoError();
    checked(EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr));
    checked(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr));
    checked(EVP_DecryptInit_ex(context.get(), nullptr, nullptr, raw(key), raw(nonce)));
    int length = 0;
    if (!aad.empty()) checked(EVP_DecryptUpdate(context.get(), nullptr, &length, raw(aad), static_cast<int>(aad.size())));
    SecretBytes tentative(ciphertext.size() + 16);
    int written = 0;
    checked(EVP_DecryptUpdate(context.get(), raw(tentative.bytes()), &written, raw(ciphertext), static_cast<int>(ciphertext.size())));
    if (written < 0 || static_cast<std::size_t>(written) != ciphertext.size()) throw CryptoError();
    auto expectedTag = tag;
    checked(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, 16, expectedTag.data()));
    int finalLength = 0;
    checked(EVP_DecryptFinal_ex(context.get(), raw(tentative.bytes().subspan(static_cast<std::size_t>(written))), &finalLength));
    if (finalLength != 0) throw CryptoError();
    // No tentative byte is returned or parsed before final authentication.
    SecretBytes authenticated(ciphertext.size());
    if (!ciphertext.empty()) std::copy_n(tentative.bytes().begin(), ciphertext.size(), authenticated.bytes().begin());
    return authenticated;
}

SecretBytes derivePasswordKey(std::string_view password, std::span<const std::byte, 16> salt) {
    if (validatePassword(password) != PasswordStatus::Valid) throw CryptoError();
    static_assert(ARGON2_VERSION_NUMBER == 0x13);
    static std::counting_semaphore<2> slots(2);
    slots.acquire();
    struct Release { std::counting_semaphore<2>& slots; ~Release() { slots.release(); } } release{slots};
    SecretBytes key(32);
    if (argon2id_hash_raw(3, 65536, 1, password.data(), password.size(), salt.data(), salt.size(),
                          key.bytes().data(), key.bytes().size()) != ARGON2_OK) throw CryptoError();
    return key;
}

std::vector<std::byte> createEnvelope(std::string_view password, std::span<const std::byte> payload) {
    if (validatePassword(password) != PasswordStatus::Valid || payload.empty() || payload.size() > MaxPlaintextBytes)
        throw CryptoError();
    Header header{};
    constexpr std::string_view magic{"NORDVLT\0", 8};
    for (std::size_t i = 0; i < magic.size(); ++i) header[i] = std::byte(magic[i]);
    writeLe(header, 8, 1, 4); writeLe(header, 12, HeaderSize, 4);
    random(std::span(header).subspan(24, 16));
    header[30] = (header[30] & std::byte{0x0f}) | std::byte{0x40};
    header[32] = (header[32] & std::byte{0x3f}) | std::byte{0x80};
    writeLe(header, 40, 1, 4);
    random(std::span(header).subspan(44, 16));
    random(std::span(header).subspan(60, 12));
    writeLe(header, 140, 1, 8); writeLe(header, 148, 1, 8);
    SecretBytes key(32);
    random(key.bytes(), true);
    const auto kek = derivePasswordKey(password, std::span<const std::byte,16>(header.data()+44,16));
    const auto wrapped = encryptGcm(asKey(kek), nonceAt(header, 60), key.bytes(), wrapAad(header));
    std::copy(wrapped.bytes.begin(), wrapped.bytes.end(), header.begin()+72);
    std::copy(wrapped.tag.begin(), wrapped.tag.end(), header.begin()+104);
    return seal(header, asKey(key), payload);
}

OpenedEnvelope openEnvelopeWithKey(std::span<const std::byte> file, KeyView key) {
    const auto header = admittedHeader(file);
    const auto check = validateHeader(header, file.size());
    Tag tag{};
    std::copy_n(file.end()-TagSize, TagSize, tag.begin());
    auto payload = decryptGcm(key, nonceAt(header,120), file.subspan(HeaderSize, static_cast<std::size_t>(check.payloadLength)), dataAad(header), tag);
    SecretBytes ownedKey(32);
    std::copy(key.begin(), key.end(), ownedKey.bytes().begin());
    return {header, std::move(ownedKey), std::move(payload)};
}

OpenedEnvelope openEnvelope(std::span<const std::byte> file, std::string_view password) {
    const auto header = admittedHeader(file); // Layout rejected BEFORE Argon2.
    const auto kek = derivePasswordKey(password, std::span<const std::byte,16>(header.data()+44,16));
    Tag tag{};
    std::copy_n(header.begin()+104, TagSize, tag.begin());
    const auto key = decryptGcm(asKey(kek), nonceAt(header,60), std::span(header).subspan(72,32), wrapAad(header), tag);
    return openEnvelopeWithKey(file, asKey(key));
}

std::vector<std::byte> resealEnvelope(const OpenedEnvelope& opened, std::span<const std::byte> payload) {
    const auto check = validateHeader(opened.header, HeaderSize + opened.payload.bytes().size() + TagSize);
    if (!check.valid() || check.revision == 0x7fffffffffffffffULL ||
        check.revision - check.keyRevision + 1 >= KeyRevisionLimit) throw CryptoError();
    auto header = opened.header;
    writeLe(header,140,check.revision+1,8);
    return seal(header, asKey(opened.key), payload);
}
} // namespace nordssh::vault
