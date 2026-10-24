#include "ByteQueue.h"
#include <algorithm>

namespace nordssh::ssh {
bool ByteQueue::tryPush(std::span<const std::byte> bytes) {
    std::lock_guard lock(mutex_);
    if (closed_ || bytes.empty() || bytes.size() > MaxChunk ||
        chunks_.size() >= MaxChunks || bytes.size() > MaxBytes - bytes_) return false;
    vault::SecretBytes chunk(bytes.size());
    std::ranges::copy(bytes, chunk.bytes().begin());
    chunks_.push_back(std::move(chunk));
    bytes_ += bytes.size();
    return true;
}
std::optional<vault::SecretBytes> ByteQueue::tryPop() {
    std::lock_guard lock(mutex_);
    if (chunks_.empty()) return {};
    auto chunk = std::move(chunks_.front());
    chunks_.pop_front();
    bytes_ -= chunk.bytes().size();
    return chunk;
}
bool ByteQueue::roomForChunk() const {
    std::lock_guard lock(mutex_);
    return !closed_ && chunks_.size() < MaxChunks && bytes_ <= MaxBytes - MaxChunk;
}
ByteQueue::Usage ByteQueue::usage() const {
    std::lock_guard lock(mutex_);
    return {bytes_, chunks_.size(), closed_};
}
void ByteQueue::close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    chunks_.clear();
    bytes_ = 0;
}
} // namespace nordssh::ssh
