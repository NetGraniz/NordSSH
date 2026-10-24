#include "ssh/JumpConnection.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <future>
#include <iostream>
#include <cstring>
#include <algorithm>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#endif
using namespace nordssh::ssh;
using namespace std::chrono_literals;
static void require(bool value) { if (!value) throw std::runtime_error("Forwarding/jump contract failed"); }
static std::string hex(const HostIdentity& identity) { return QByteArray(reinterpret_cast<const char*>(identity.sha256.data()),32).toHex().toStdString(); }
static void advance(ConnectionManager& manager, const std::string& hash, const char* user, const char* password, bool& authenticated, bool reject = false) {
    if (auto prompt = manager.hostPrompt()) {
        require(hex(prompt->identity) == hash && prompt->identity.endpoint.user == user);
        if (reject) require(manager.reject(prompt->attempt));
        else require(manager.approve(prompt->attempt,prompt->challenge));
    }
    const auto status = manager.status();
    if (status.phase == AttemptPhase::Authentication && !authenticated) {
        const auto size = std::strlen(password); nordssh::vault::SecretBytes secret(size); std::memcpy(secret.bytes().data(),password,size);
        require(manager.authenticate(*status.request,{Credentials::Kind::Password,std::move(secret)})); authenticated = true;
    }
}
static void echo(std::uint16_t port) {
    const auto fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); require(fd != SSH_INVALID_SOCKET);
    struct Cleanup { socket_t fd; ~Cleanup() {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    } } cleanup{fd};
#ifdef _WIN32
    const DWORD timeout = 5000;
#else
    const timeval timeout{5,0};
#endif
    require(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout)));
    require(!setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout)));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    require(!::connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address)));
    std::string expected; for (unsigned i = 0; i < 16384; ++i) expected += static_cast<char>(i % 251);
    std::size_t sent = 0;
    while (sent < expected.size()) { const auto n = send(fd,expected.data()+sent,static_cast<int>(expected.size()-sent),0); require(n > 0); sent += static_cast<std::size_t>(n); }
#ifdef _WIN32
    require(!shutdown(fd,SD_SEND));
#else
    require(!shutdown(fd,SHUT_WR));
#endif
    std::string actual; std::array<char,4096> bytes{};
    for (;;) { const auto count = recv(fd,bytes.data(),static_cast<int>(bytes.size()),0); require(count >= 0); if (!count) break; actual.append(bytes.data(),static_cast<std::size_t>(count)); require(actual.size() <= 20000); }
    require(actual == expected + "tail-after-eof");
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc == 7);
        const auto jumpPort = static_cast<std::uint16_t>(std::stoul(argv[1])), targetPort = static_cast<std::uint16_t>(std::stoul(argv[3])), echoPort = static_cast<std::uint16_t>(std::stoul(argv[5]));
        const std::string jumpHash = argv[2], targetHash = argv[4], mode = argv[6];
        ConnectionRuntime runtime;
        if (mode == "forward") {
            auto forwarder = std::make_shared<LocalForwarder>("127.0.0.1",echoPort);
            ConnectionManager manager(runtime); require(manager.start({"localhost",jumpPort,"jump-user"},{},{},80,24,forwarder));
            bool authenticated = false, launched = false; std::vector<std::future<void>> clients; QElapsedTimer timer; timer.start();
            while (timer.elapsed() < 20000) {
                QCoreApplication::processEvents(); advance(manager,jumpHash,"jump-user","jump-synthetic-password",authenticated);
                const auto state = forwarder->status();
                if (state.port && !launched) { for (unsigned i=0;i<4;++i) clients.push_back(std::async(std::launch::async,[port=state.port] {echo(port);})); launched=true; }
                if (launched && std::ranges::all_of(clients,[](auto& client) {return client.wait_for(0s)==std::future_status::ready;})) break;
                require(!manager.status().settled); QThread::msleep(2);
            }
            require(launched); for (auto& client:clients) client.get(); manager.cancel();
            while (!manager.status().settled && timer.elapsed()<22000) {QCoreApplication::processEvents();QThread::msleep(2);}
            require(manager.status().settled && forwarder->status().stopped);
        } else {
            JumpConnection connection(runtime);
            std::optional<TrustedHostPin> pin;
            if (mode=="changed-target") pin=TrustedHostPin{"destination.invalid",targetPort,"ssh-ed25519",{}};
            require(connection.start({"localhost",jumpPort,"jump-user"},{"destination.invalid",targetPort,"dest-user"},{},pin));
            bool jumpAuth=false,targetAuth=false,sent=false,observed=false;std::string output;QElapsedTimer timer;timer.start();
            while(timer.elapsed()<20000) {
                QCoreApplication::processEvents(); advance(connection.jump(),jumpHash,"jump-user","jump-synthetic-password",jumpAuth,mode=="reject-jump");
                if(auto prompt=connection.destination().hostPrompt()) require(prompt->identity.endpoint.host=="destination.invalid"&&prompt->identity.endpoint.port==targetPort);
                advance(connection.destination(),targetHash,"dest-user","dest-synthetic-password",targetAuth);
                const auto state=connection.destination().status();
                if(state.phase==AttemptPhase::Active&&!sent) {const std::string text="through-jump\n";require(connection.destination().send(std::as_bytes(std::span(text))));sent=true;}
                while(auto bytes=connection.destination().takeOutput()) {output.append(reinterpret_cast<const char*>(bytes->bytes().data()),bytes->bytes().size());require(output.size()<65536);}
                if(output.find("echo:through-jump\n")!=output.npos&&!observed) {observed=true;connection.cancel();}
                if(connection.settled()) break;
                QThread::msleep(2);
            }
            require(connection.settled());
            if(mode=="jump") require(jumpAuth&&targetAuth&&sent&&observed);
            else if(mode=="reject-jump") require(!jumpAuth&&!targetAuth&&!sent);
            else if(mode=="changed-target") require(jumpAuth&&!targetAuth&&!sent&&connection.destination().status().reason==AttemptReason::ChangedKey);
            else require(false);
        }
        runtime.shutdown();QElapsedTimer timer;timer.start();while(!runtime.idle()&&timer.elapsed()<3000){QCoreApplication::processEvents();QThread::msleep(2);}require(runtime.idle());
        std::cout<<"Forwarding fixture mode passed: "<<mode<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
