#pragma once
#include "vault/VaultCrypto.h"
#include <deque>
#include <mutex>
#include <optional>

namespace nordssh::ssh {
// A session-local bounded mailbox. SecretBytes also cleanses pasted terminal
// passwords. Queue close discards pending bytes; it never flushes them remotely.
class ByteQueue final {
public:
    static constexpr std::size_t MaxBytes = 1024 * 1024, MaxChunks = 128, MaxChunk = 65536;
    struct Usage { std::size_t bytes, chunks; bool closed; };
    ByteQueue() = default;
    ByteQueue(const ByteQueue&) = delete;
    ByteQueue& operator=(const ByteQueue&) = delete;
    // Rejection is atomic: no truncated paste and no allocation for rejected data.
    [[nodiscard]] bool tryPush(std::span<const std::byte> bytes);
    [[nodiscard]] std::optional<vault::SecretBytes> tryPop();
    [[nodiscard]] bool roomForChunk() const;
    [[nodiscard]] Usage usage() const;
    void close();
private:
    mutable std::mutex mutex_;
    std::deque<vault::SecretBytes> chunks_;
    std::size_t bytes_ = 0;
    bool closed_ = false;
};
} // namespace nordssh::ssh
