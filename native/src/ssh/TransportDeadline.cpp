#include "TransportDeadline.h"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
namespace nordssh::ssh {
socket_t TransportDeadline::socketFor(Session& session, OperationControl control) {
    session.owner();
    if (session.state_ != Session::State::Authenticated && session.state_ != Session::State::Service) throw SessionError();
    if (control.stop.stop_requested() || std::chrono::steady_clock::now() >= control.deadline ||
        control.deadline - std::chrono::steady_clock::now() > std::chrono::hours(24)) throw SessionError();
    const auto socket = ssh_get_fd(session.session_);
    if (socket == SSH_INVALID_SOCKET) throw SessionError();
    return socket;
}
TransportDeadline::TransportDeadline(Session& session, OperationControl control)
    : session_(session), control_(control), socket_(socketFor(session, control)),
      cancel_(control.stop, [this] { interrupt(); }) {
    if (interrupted_.load() || control.stop.stop_requested()) throw SessionError();
    timer_ = std::jthread([this](std::stop_token stop) {
        std::unique_lock lock(mutex_);
        if (!wake_.wait_until(lock, stop, control_.deadline, [] { return false; }) && !stop.stop_requested()) interrupt();
    });
}
void TransportDeadline::interrupt() noexcept {
    interrupted_.store(true);
    if (socket_ == SSH_INVALID_SOCKET) return;
#ifdef _WIN32
    (void)shutdown(socket_, SD_BOTH);
#else
    (void)shutdown(socket_, SHUT_RDWR);
#endif
}
TransportDeadline::~TransportDeadline() {
    timer_.request_stop(); wake_.notify_all(); if (timer_.joinable()) timer_.join();
    if (interrupted_.load()) session_.state_ = Session::State::Failed;
}
void TransportDeadline::check() const {
    if (interrupted_.load() || control_.stop.stop_requested() || std::chrono::steady_clock::now() >= control_.deadline) throw SessionError();
}
} // namespace nordssh::ssh
