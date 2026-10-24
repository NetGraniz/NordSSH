#pragma once
#include "ByteQueue.h"
#include "Session.h"

namespace nordssh::ssh {
// Single session-worker owner. Queues outlive the pump; input has one producer,
// output one consumer. No callbacks, GUI objects or unbounded event queues.
// The future worker scheduler calls step and waits when it reports Idle/Blocked.
class ShellPump final {
public:
    enum class Result { Progress, Idle, Blocked, Closed };
    ShellPump(Session& session, ByteQueue& input, ByteQueue& output);
    [[nodiscard]] Result step(std::stop_token stop = {});
private:
    Session& session_;
    ByteQueue& input_;
    ByteQueue& output_;
    std::optional<vault::SecretBytes> pending_;
    std::size_t offset_ = 0;
    // Cleansed at worker teardown; reused rather than allocating for idle reads.
    vault::SecretBytes scratch_{ByteQueue::MaxChunk};
    std::thread::id owner_ = std::this_thread::get_id();
};
} // namespace nordssh::ssh
