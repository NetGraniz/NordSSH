#include "ShellPump.h"
#include <algorithm>

namespace nordssh::ssh {
ShellPump::ShellPump(Session& session, ByteQueue& input, ByteQueue& output)
    : session_(session), input_(input), output_(output) {}
ShellPump::Result ShellPump::step(std::stop_token stop) {
    if (owner_ != std::this_thread::get_id()) throw SessionError();
    // Stop/control is checked before data capacity: a flooded output cannot
    // starve cancellation. Discard rather than replay queued terminal input.
    if (stop.stop_requested() || input_.usage().closed || output_.usage().closed) {
        pending_.reset();
        input_.close();
        return Result::Closed;
    }
    // Even flush/write may receive packets into libssh. Stop ALL transport
    // pumping while the UI queue is full, not merely copying stdout to the UI.
    if (!output_.roomForChunk()) return Result::Blocked;
    bool progress = false;
    for (const bool standardError : {false, true}) {
        if (!output_.roomForChunk()) break;
        const auto count = session_.readSome(scratch_.bytes(), standardError, stop);
        if (count) {
            if (!output_.tryPush(scratch_.bytes().first(count))) return Result::Closed;
            progress = true;
        }
    }
    if (session_.closed()) {
        pending_.reset();
        input_.close();
        return Result::Closed; // Output remains available for the UI to drain.
    }
    if (!output_.roomForChunk()) return progress ? Result::Progress : Result::Blocked;
    if (!pending_) { pending_ = input_.tryPop(); offset_ = 0; }
    if (pending_) {
        const auto remaining = pending_->bytes().subspan(offset_);
        const auto count = session_.writeSome(remaining.first(std::min<std::size_t>(remaining.size(), 16384)), stop);
        offset_ += count;
        if (offset_ == pending_->bytes().size()) pending_.reset();
        progress = progress || count != 0;
    } else {
        (void)session_.flushSome(stop);
    }
    return progress ? Result::Progress : Result::Idle;
}
} // namespace nordssh::ssh
