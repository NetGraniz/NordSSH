#include "ssh/Session.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

using nordssh::ssh::Session;
using nordssh::ssh::SessionError;
static void require(bool result,const char* message) { if(!result) throw std::runtime_error(message); }
template<class Action> static void reject(Action action,const char* message) {
    try { action(); } catch(const SessionError&) { return; }
    throw std::runtime_error(message);
}
static std::string fingerprint(const std::array<std::byte,32>& bytes) {
    constexpr std::string_view hex="0123456789abcdef";
    std::string output;
    for(const auto byte:bytes) {
        const auto value=std::to_integer<unsigned>(byte);
        output+=hex[value>>4]; output+=hex[value&15];
    }
    return output;
}
int main(int argc,char** argv) {
    try {
        require(argc==4,"Fixture arguments: port, public SHA256 hex, mode");
        const auto port=std::stoi(argv[1]);
        require(port>1024 && port<=65535,"Fixture port outside loopback test range");
        const std::string mode=argv[3];
        Session client;
        client.connectResolved({"127.0.0.1",static_cast<std::uint16_t>(port),"fixture-user"}, "127.0.0.1");
        auto identity=client.hostIdentity();
        require(fingerprint(identity.sha256)==argv[2],"Fixture host fingerprint mismatch");
        if(mode=="untrusted") {
            reject([&]{client.authenticatePassword("synthetic-ssh-password");},"Authentication before host approval");
        } else if(mode=="changed") {
            identity.sha256[0]^=std::byte{1};
            reject([&]{client.authorizeHost(identity);},"Changed host key accepted");
            reject([&]{client.authenticatePassword("synthetic-ssh-password");},"Authentication after changed key rejection");
        } else if(mode=="replay") {
            Session other;
            other.connectResolved(identity.endpoint, "127.0.0.1");
            reject([&]{other.authorizeHost(identity);},"Host decision replayed across connections");
        } else {
            client.authorizeHost(identity);
            if(mode=="wrong-password") {
                reject([&]{client.authenticatePassword("synthetic-wrong-password");},"Wrong SSH password accepted");
            } else {
                require(mode=="shell","Unknown fixture mode");
                client.authenticatePassword("synthetic-ssh-password");
                client.openShell(80,24);
                client.resize(100,40);
                std::string output;
                const auto readyDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                while(output.find("fixture-ready")==std::string::npos && std::chrono::steady_clock::now()<readyDeadline) {
                    const auto data=client.read(100);
                    output.append(reinterpret_cast<const char*>(data.data()),data.size());
                    require(output.size()<65536,"Unbounded fixture ready output");
                }
                require(output.find("fixture-ready")!=std::string::npos,"No initial shell output");
                require(client.read(100).empty(),"Idle read must not fail or invent output");
                const std::string payload="once-\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\n";
                client.write(std::as_bytes(std::span(payload)));
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(output.find("echo:"+payload)==std::string::npos && std::chrono::steady_clock::now()<deadline) {
                    const auto data=client.read(100);
                    output.append(reinterpret_cast<const char*>(data.data()),data.size());
                    require(output.size()<65536,"Unbounded fixture output");
                }
                require(output.find("fixture-ready")!=std::string::npos,"No real shell output");
                require(output.find("echo:"+payload)!=std::string::npos,"UTF8 shell bytes changed");
            }
        }
        std::cout<<"SSH fixture "<<mode<<" passed\n";
        return EXIT_SUCCESS;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return EXIT_FAILURE; }
}
