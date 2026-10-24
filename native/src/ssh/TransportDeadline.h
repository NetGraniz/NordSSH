#pragma once
#include "Session.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace nordssh::ssh {
// Deadline/cancel for libssh's blocking SFTP APIs. Only shutdown(2)/Winsock
// shutdown may run off-owner; no libssh call and no descriptor close/reuse race.
class TransportDeadline final {
public:
    TransportDeadline(Session&, OperationControl);
    ~TransportDeadline();
    void check() const;
private:
    Session& session_;
    OperationControl control_;
    socket_t socket_;
    std::atomic<bool> interrupted_{false};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::stop_callback<std::function<void()>> cancel_;
    std::jthread timer_;
    void interrupt() noexcept;
    static socket_t socketFor(Session&, OperationControl);
};
} // namespace nordssh::ssh
