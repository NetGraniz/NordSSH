#include "DirectChannel.h"
#include <algorithm>
namespace nordssh::ssh {
DirectChannel::DirectChannel(Session& session, const std::string& host, std::uint16_t port, OperationControl control) : session_(session) {
    session_.owner();
    if ((session_.state_ != Session::State::Authenticated && session_.state_ != Session::State::Service) ||
        host.empty() || host.size() > 253 || !port || host.find('\0') != host.npos) throw SessionError();
    session_.state_ = Session::State::Service;
    channel_ = ssh_channel_new(session_.session_); if (!channel_) throw SessionError();
    try {
        if (session_.retry([&] { return ssh_channel_open_forward(channel_,host.c_str(),port,"127.0.0.1",0); }, SSH_AGAIN,control) != SSH_OK) throw SessionError();
    } catch (...) { ssh_channel_free(channel_); channel_ = nullptr; throw; }
}
DirectChannel::~DirectChannel() { if (channel_) ssh_channel_free(channel_); }
std::size_t DirectChannel::read(std::span<std::byte> bytes) {
    session_.require(Session::State::Service);
    if (bytes.empty() || bytes.size() > 16384) throw SessionError();
    const auto result = ssh_channel_read_nonblocking(channel_,bytes.data(),static_cast<std::uint32_t>(bytes.size()),0);
    if (result == SSH_AGAIN || result == SSH_EOF) return 0;
    if (result < 0 || static_cast<std::size_t>(result) > bytes.size()) throw SessionError();
    return static_cast<std::size_t>(result);
}
std::size_t DirectChannel::write(std::span<const std::byte> bytes) {
    session_.require(Session::State::Service);
    if (bytes.empty() || bytes.size() > 16384) throw SessionError();
    if (!session_.flushSome()) return 0;
    const auto result = ssh_channel_write(channel_,bytes.data(),static_cast<std::uint32_t>(bytes.size()));
    if (result == SSH_AGAIN) return 0;
    if (result < 0 || static_cast<std::size_t>(result) > bytes.size()) throw SessionError();
    return static_cast<std::size_t>(result);
}
bool DirectChannel::sendEof() {
    session_.require(Session::State::Service);
    if (!session_.flushSome()) return false;
    const auto result = ssh_channel_send_eof(channel_);
    if (result == SSH_AGAIN) return false;
    if (result != SSH_OK) throw SessionError();
    return true;
}
bool DirectChannel::eof() const { session_.require(Session::State::Service); return ssh_channel_is_eof(channel_) || ssh_channel_is_closed(channel_); }
} // namespace nordssh::ssh
