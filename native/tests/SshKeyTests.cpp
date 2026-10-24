#include "ssh/Session.h"
#include "vault/VaultCrypto.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <cstdlib>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
using namespace nordssh;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static vault::SecretBytes field(std::size_t maximum) {
    std::array<unsigned char,4> header{};
    std::cin.read(reinterpret_cast<char*>(header.data()),4);require(bool(std::cin),"Fixture frame header");
    std::uint32_t size=0;for(unsigned i=0;i<4;++i)size|=std::uint32_t(header[i])<<(8*i);
    require(size<=maximum,"Fixture frame exceeds bound");vault::SecretBytes result(size);
    if(size)std::cin.read(reinterpret_cast<char*>(result.bytes().data()),size);
    require(bool(std::cin),"Fixture frame truncated");return result;
}
static std::string_view view(const vault::SecretBytes& value) {
    return {reinterpret_cast<const char*>(value.bytes().data()),value.bytes().size()};
}
static std::string fingerprint(const std::array<std::byte,32>& hash) {
    constexpr char hex[]="0123456789abcdef";std::string output;
    for(auto byte:hash){const auto n=std::to_integer<unsigned>(byte);output+=hex[n>>4];output+=hex[n&15];}return output;
}
int main(int argc,char** argv) {
    try {
#ifdef _WIN32
        require(_setmode(_fileno(stdin),_O_BINARY)!=-1,"Fixture binary stdin");
#endif
        require(argc==4,"Isolated key fixture arguments required");
        const auto port=std::stoi(argv[1]);require(port>1024&&port<=65535,"Loopback test port out of range");
        const std::string mode=argv[3];
        auto key=field(1048576),phrase=field(4096),certificate=field(1048576);
        ssh::Session client;client.connectResolved({"127.0.0.1",static_cast<std::uint16_t>(port),"fixture-user"}, "127.0.0.1");
        auto identity=client.hostIdentity();require(fingerprint(identity.sha256)==argv[2],"Fixture host key mismatch");
        if(mode!="untrusted")client.authorizeHost(identity);
        const bool expectedFailure=mode=="untrusted"||mode=="wrong-phrase"||mode=="bad-format"||mode=="high-cost"||
            mode=="cert-mismatch"||mode=="expired"||mode=="principal"||mode=="denied"||mode=="weak-rsa"||mode.starts_with("reject-");
        bool rejected=false;
        try{client.authenticatePrivateKey(view(key),view(phrase),view(certificate));}catch(const ssh::SessionError&){rejected=true;}
        require(rejected==expectedFailure,"Unexpected key authentication outcome");
        if(!rejected) {
            client.openShell(80,24);
            const std::string payload="key-auth-ok\n";client.write(std::as_bytes(std::span(payload)));
            std::string received;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(received.find("echo:"+payload)==std::string::npos&&std::chrono::steady_clock::now()<deadline) {
                const auto bytes=client.read(100);received.append(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                require(received.size()<65536,"Unbounded key fixture output");
            }
            require(received.find("echo:"+payload)!=std::string::npos,"No authenticated key shell echo");
        }
        std::cout<<"Synthetic SSH key case "<<mode<<" passed\n";return EXIT_SUCCESS;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return EXIT_FAILURE;}
}
