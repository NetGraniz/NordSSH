#include "LocalForwarder.h"
#include "DirectChannel.h"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif
namespace nordssh::ssh {
namespace {
using namespace std::chrono_literals;
bool wouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
class Socket {
public:
    socket_t fd = SSH_INVALID_SOCKET;
    explicit Socket(socket_t value) : fd(value) {}
    Socket(Socket&& other) noexcept : fd(std::exchange(other.fd, SSH_INVALID_SOCKET)) {}
    Socket& operator=(Socket&&) = delete;
    Socket(const Socket&) = delete;
    ~Socket() {
        if (fd != SSH_INVALID_SOCKET) {
#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
        }
    }
    void nonblocking() const {
#ifdef _WIN32
        u_long enabled = 1; if (ioctlsocket(fd,FIONBIO,&enabled)) throw SessionError();
        if (!SetHandleInformation(reinterpret_cast<HANDLE>(fd),HANDLE_FLAG_INHERIT,0)) throw SessionError();
#else
        const auto flags = fcntl(fd,F_GETFL,0);
        if (flags < 0 || fcntl(fd,F_SETFL,flags | O_NONBLOCK) < 0 || fcntl(fd,F_SETFD,FD_CLOEXEC) < 0) throw SessionError();
#endif
    }
    void finishOutput() const {
#ifdef _WIN32
        (void)shutdown(fd,SD_SEND);
#else
        (void)shutdown(fd,SHUT_WR);
#endif
    }
};
struct Stream {
    Socket local;
    DirectChannel channel;
    vault::SecretBytes toRemote{16384}, toLocal{16384};
    std::size_t remoteAt = 0, remoteSize = 0, localAt = 0, localSize = 0;
    bool localEof = false, remoteEof = false, sentEof = false;
    Stream(Socket socket, Session& session, const std::string& host, std::uint16_t port, std::stop_token stop)
        : local(std::move(socket)), channel(session,host,port,{stop,std::chrono::steady_clock::now()+5s}) {}
    bool step(bool& progress) {
        if (!localEof && !remoteSize) {
            const auto amount = recv(local.fd,reinterpret_cast<char*>(toRemote.bytes().data()),16384,0);
            if (amount > 0) { remoteSize = static_cast<std::size_t>(amount); remoteAt = 0; progress = true; }
            else if (!amount) { localEof = true; progress = true; }
            else if (!wouldBlock()) return false;
        }
        if (remoteSize) {
            const auto amount = channel.write(toRemote.bytes().subspan(remoteAt,remoteSize));
            remoteAt += amount; remoteSize -= amount; progress = progress || amount != 0;
        }
        if (localEof && !remoteSize && !sentEof) { sentEof = channel.sendEof(); progress = progress || sentEof; }
        if (!remoteEof && !localSize) {
            localSize = channel.read(toLocal.bytes()); localAt = 0; progress = progress || localSize != 0;
            if (!localSize && channel.eof()) { remoteEof = true; local.finishOutput(); progress = true; }
        }
        if (localSize) {
#ifdef _WIN32
            constexpr int flags = 0;
#else
            constexpr int flags = MSG_NOSIGNAL;
#endif
            const auto amount = send(local.fd,reinterpret_cast<const char*>(toLocal.bytes().data()+localAt),static_cast<int>(localSize),flags);
            if (amount > 0) { localAt += static_cast<std::size_t>(amount); localSize -= static_cast<std::size_t>(amount); progress = true; }
            else if (!amount || !wouldBlock()) return false;
        }
        return !(localEof && remoteEof && sentEof && !localSize && !remoteSize);
    }
};
}
LocalForwarder::LocalForwarder(std::string host, std::uint16_t remotePort, std::uint16_t localPort)
    : destination_(std::move(host)), remotePort_(remotePort), requestedPort_(localPort) {
    const auto canonical = HostResolver::canonicalHost(destination_);
    if (!canonical || !remotePort_) throw SessionError();
    destination_ = *canonical;
}
ForwardStatus LocalForwarder::status() const { return {boundPort_.load(),connections_.load(),stopped_.load()}; }
void LocalForwarder::run(Session& session, std::stop_token stop) {
    if (used_.exchange(true)) throw SessionError();
    try {
        Socket listener(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
        if (listener.fd == SSH_INVALID_SOCKET) throw SessionError();
#ifdef _WIN32
        const int exclusive = 1;
        if (setsockopt(listener.fd,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))) throw SessionError();
#endif
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(requestedPort_);
        if (bind(listener.fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address)) || listen(listener.fd,16)) throw SessionError();
        listener.nonblocking();
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        if (getsockname(listener.fd,reinterpret_cast<sockaddr*>(&address),&length)) throw SessionError();
        boundPort_.store(ntohs(address.sin_port));
        std::vector<std::unique_ptr<Stream>> streams;
        bool serviceActivated = false;
        std::mutex mutex; std::condition_variable_any wake;
        auto lastKeepalive = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            bool progress = false;
            for (unsigned count = 0; count < 16; ++count) {
                const auto fd = accept(listener.fd,nullptr,nullptr);
                if (fd == SSH_INVALID_SOCKET) { if (!wouldBlock()) throw SessionError(); break; }
                Socket accepted(fd); accepted.nonblocking(); progress = true;
                if (streams.size() == 16) continue;
                try { streams.push_back(std::make_unique<Stream>(std::move(accepted),session,destination_,remotePort_,stop)); serviceActivated = true; }
                catch (const SessionError&) { if (stop.stop_requested()) break; }
            }
            for (auto it = streams.begin(); it != streams.end();) {
                try { if ((*it)->step(progress)) { ++it; continue; } }
                catch (const SessionError&) { if (stop.stop_requested()) break; }
                it = streams.erase(it); progress = true;
            }
            connections_.store(static_cast<unsigned>(streams.size()));
            if (!session.transportActive()) throw SessionError();
            if (serviceActivated) (void)session.flushSome(stop);
            if (std::chrono::steady_clock::now() - lastKeepalive >= 30s) {
                session.keepAlive({stop,std::chrono::steady_clock::now()+2s}); lastKeepalive = std::chrono::steady_clock::now();
            }
            if (!progress) { std::unique_lock lock(mutex); wake.wait_for(lock,stop,5ms,[] {return false;}); }
        }
    } catch (...) { boundPort_ = 0; connections_ = 0; stopped_ = true; throw; }
    boundPort_ = 0; connections_ = 0; stopped_ = true;
}
} // namespace nordssh::ssh
