#include "DeviceKeys.h"
#include <QCoreApplication>
#include <QThread>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <cstring>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#else
#include <libsecret/secret.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace nordssh::vault {
namespace {
void check(const QString& identity) {
    static const QRegularExpression pattern(QStringLiteral("^[a-f0-9]{64}$"));
    if (!pattern.match(identity).hasMatch() || (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread())) throw DeviceError();
}
#ifdef _WIN32
struct Blob {
    DATA_BLOB value{};
    ~Blob() { if (value.pbData) { SecureZeroMemory(value.pbData, value.cbData); LocalFree(value.pbData); } }
};
#else
struct Call {
    GCancellable* cancel = g_cancellable_new();
    GError* error{};
    SecretSchema* schema = secret_schema_new("com.nordssh.native.device.v1", SECRET_SCHEMA_NONE,
        "identity", SECRET_SCHEMA_ATTRIBUTE_STRING, nullptr);
    std::mutex mutex;
    std::condition_variable_any condition;
    std::jthread timer{[this](std::stop_token stop) {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, stop, std::chrono::seconds(30), [] { return false; }) && !stop.stop_requested()) g_cancellable_cancel(cancel);
    }};
    ~Call() { timer.request_stop(); condition.notify_all(); timer.join(); if (error) g_error_free(error); secret_schema_unref(schema); g_object_unref(cancel); }
};
#endif
}
QString deviceIdentity(const QString& path, const Header& header) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("NordSSH-native-device-v1\0"));
    hash.addData(path.toUtf8());
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(header.data() + 24), 16));
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(header.data() + 148), 8));
    return QString::fromLatin1(hash.result().toHex());
}
void DeviceKeys::remember(const QString& identity, KeyView key) const {
    check(identity);
#ifdef _WIN32
    const auto entropyBytes = identity.toLatin1();
    DATA_BLOB input{32, reinterpret_cast<BYTE*>(const_cast<std::byte*>(key.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    Blob output;
    if (!CryptProtectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.value)) throw DeviceError();
    files_.write(identity + QStringLiteral(".device"), std::span(reinterpret_cast<const std::byte*>(output.value.pbData), output.value.cbData));
#else
    SecretBytes encoded(65);
    constexpr char hex[] = "0123456789abcdef";
    auto* text = reinterpret_cast<char*>(encoded.bytes().data());
    for (std::size_t i = 0; i < 32; ++i) { const auto b = std::to_integer<unsigned>(key[i]); text[2*i] = hex[b >> 4]; text[2*i+1] = hex[b & 15]; }
    Call call;
    if (!secret_password_store_sync(call.schema, SECRET_COLLECTION_DEFAULT, "NordSSH device unlock", text, call.cancel, &call.error,
        "identity", identity.toLatin1().constData(), nullptr) || call.error) throw DeviceError();
#endif
}
std::optional<SecretBytes> DeviceKeys::lookup(const QString& identity) const {
    check(identity);
#ifdef _WIN32
    const auto name = identity + QStringLiteral(".device");
    if (!files_.exists(name)) return std::nullopt;
    auto bytes = files_.read(name);
    if (bytes.empty() || bytes.size() > 65536) throw DeviceError();
    const auto entropyBytes = identity.toLatin1();
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes.constData()))};
    Blob output;
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.value) || output.value.cbData != 32) throw DeviceError();
    SecretBytes key(32); std::memcpy(key.bytes().data(), output.value.pbData, 32); return key;
#else
    Call call;
    auto* value = secret_password_lookup_sync(call.schema, call.cancel, &call.error, "identity", identity.toLatin1().constData(), nullptr);
    if (call.error) { if (value) secret_password_free(value); throw DeviceError(); }
    if (!value) return std::nullopt;
    SecretBytes key(32); bool valid = true;
    std::size_t length = 0; while (length < 65 && value[length]) ++length;
    if (length != 64) valid = false;
    for (std::size_t i = 0; valid && i < 32; ++i) {
        unsigned byte = 0;
        for (unsigned j = 0; j < 2; ++j) {
            const char c = value[2*i+j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { valid = false; break; }
            byte = byte * 16 + static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        key.bytes()[i] = std::byte(byte);
    }
    secret_password_free(value);
    if (!valid) throw DeviceError();
    return key;
#endif
}
void DeviceKeys::forget(const QString& identity) const {
    check(identity);
#ifdef _WIN32
    files_.forgetDeviceRecord(identity + QStringLiteral(".device"));
#else
    Call call;
    (void)secret_password_clear_sync(call.schema, call.cancel, &call.error, "identity", identity.toLatin1().constData(), nullptr);
    if (call.error) throw DeviceError();
#endif
}
} // namespace nordssh::vault
