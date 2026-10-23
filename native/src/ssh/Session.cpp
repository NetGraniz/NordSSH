#include "Session.h"
#include "KeyMaterial.h"
#include "vault/VaultCrypto.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <openssl/rand.h>

namespace nordssh::ssh {
namespace {
bool validText(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::ranges::none_of(value, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
void dimensions(int columns,int rows) {
    if(columns<1 || columns>1024 || rows<1 || rows>512) throw SessionError();
}
int denyImplicitPrompt(const char*, char*, std::size_t, int, int, void*) noexcept { return -1; }
vault::SecretBytes terminatedSecret(std::string_view value, std::size_t maximum) {
    if(value.size()>maximum||value.find('\0')!=std::string_view::npos)throw SessionError();
    vault::SecretBytes copy(value.size()+1);
    if(!value.empty())std::memcpy(copy.bytes().data(),value.data(),value.size());
    return copy;
}
} // namespace
Session::Session() {
    static const bool initialized = [] { if (ssh_init()!=SSH_OK) throw SessionError(); return true; }();
    (void)initialized;
    if(RAND_bytes(reinterpret_cast<unsigned char*>(connectionId_.data()),16)!=1) throw SessionError();
    session_=ssh_new();
    if (!session_) throw SessionError();
}
Session::~Session() {
    if (channel_) ssh_channel_free(channel_);
    if (session_) { ssh_disconnect(session_); ssh_free(session_); }
}
void Session::owner() const { if (owner_!=std::this_thread::get_id()) throw SessionError(); }
void Session::require(State state) const { owner(); if(state_!=state) throw SessionError(); }
void Session::checked(int result) {
    if (result!=SSH_OK) { state_=State::Failed; throw SessionError(); }
}
void Session::connect(const Endpoint& endpoint) {
    require(State::New);
    if (!validText(endpoint.host,253) || !validText(endpoint.user,128) || endpoint.port==0) throw SessionError();
    endpoint_=endpoint;
    const int processConfig=0, logging=SSH_LOG_NOLOG;
    const unsigned int port=endpoint.port;
    const long timeout=5;
    checked(ssh_options_set(session_,SSH_OPTIONS_PROCESS_CONFIG,&processConfig));
    checked(ssh_options_set(session_,SSH_OPTIONS_LOG_VERBOSITY,&logging));
    checked(ssh_options_set(session_,SSH_OPTIONS_HOST,endpoint.host.c_str()));
    checked(ssh_options_set(session_,SSH_OPTIONS_PORT,&port));
    checked(ssh_options_set(session_,SSH_OPTIONS_USER,endpoint.user.c_str()));
    checked(ssh_options_set(session_,SSH_OPTIONS_TIMEOUT,&timeout));
    checked(ssh_options_set(session_,SSH_OPTIONS_HOSTKEYS,
        "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256"));
    checked(ssh_options_set(session_,SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES,
        "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256,"
        "ssh-ed25519-cert-v01@openssh.com,ecdsa-sha2-nistp256-cert-v01@openssh.com,"
        "ecdsa-sha2-nistp384-cert-v01@openssh.com,ecdsa-sha2-nistp521-cert-v01@openssh.com,"
        "rsa-sha2-512-cert-v01@openssh.com,rsa-sha2-256-cert-v01@openssh.com"));
    const int minimumRsaBits=2048;
    checked(ssh_options_set(session_,SSH_OPTIONS_RSA_MIN_SIZE,&minimumRsaBits));
    checked(ssh_connect(session_));
    state_=State::Connected;
}
HostIdentity Session::hostIdentity() const {
    owner();
    if (state_!=State::Connected && state_!=State::Trusted) throw SessionError();
    ssh_key key=nullptr;
    if (ssh_get_server_publickey(session_,&key)!=SSH_OK) throw SessionError();
    std::unique_ptr<ssh_key_struct,decltype(&ssh_key_free)> owned(key,&ssh_key_free);
    unsigned char* hash=nullptr;
    std::size_t length=0;
    if (ssh_get_publickey_hash(key,SSH_PUBLICKEY_HASH_SHA256,&hash,&length)!=SSH_OK) throw SessionError();
    struct HashOwner { unsigned char*& hash; ~HashOwner(){ssh_clean_pubkey_hash(&hash);} } cleanup{hash};
    if (!hash || length!=32) throw SessionError();
    const char* algorithm=ssh_key_type_to_char(ssh_key_type(key));
    if (!algorithm) throw SessionError();
    HostIdentity identity{endpoint_,algorithm,{},connectionId_};
    std::memcpy(identity.sha256.data(),hash,32);
    return identity;
}
void Session::authorizeHost(const HostIdentity& approved) {
    require(State::Connected);
    const auto actual=hostIdentity();
    if (approved.endpoint.host!=actual.endpoint.host || approved.endpoint.port!=actual.endpoint.port ||
        approved.endpoint.user!=actual.endpoint.user || approved.algorithm!=actual.algorithm || approved.sha256!=actual.sha256 ||
        approved.connectionId!=actual.connectionId) {
        state_=State::Failed;
        throw SessionError();
    }
    state_=State::Trusted;
}
void Session::authenticatePassword(std::string_view password) {
    require(State::Trusted);
    if (password.empty() || password.size()>4096 || password.find('\0')!=std::string_view::npos) throw SessionError();
    vault::SecretBytes terminated(password.size()+1);
    std::memcpy(terminated.bytes().data(),password.data(),password.size());
    const auto* text=reinterpret_cast<const char*>(terminated.bytes().data());
    if (ssh_userauth_password(session_,nullptr,text)!=SSH_AUTH_SUCCESS) { state_=State::Failed; throw SessionError(); }
    state_=State::Authenticated;
}
void Session::authenticatePrivateKey(std::string_view privateKey, std::string_view passphrase,
                                    std::string_view certificate) {
    require(State::Trusted); // Before parsing keys, prompting or transmitting authentication.
    try {
        const auto admittedHash=admitOpenSshKey(privateKey);
        auto keyText=terminatedSecret(privateKey,1048576),phrase=terminatedSecret(passphrase,4096);
        ssh_key raw=nullptr;
        const auto result=ssh_pki_import_privkey_base64(reinterpret_cast<const char*>(keyText.bytes().data()),
            reinterpret_cast<const char*>(phrase.bytes().data()),&denyImplicitPrompt,nullptr,&raw);
        std::unique_ptr<ssh_key_struct,decltype(&ssh_key_free)> key(raw,&ssh_key_free);
        if(result!=SSH_OK||!key||!ssh_key_is_private(key.get()))throw SessionError();
        unsigned char* actualHash=nullptr;std::size_t hashLength=0;
        const auto hashResult=ssh_get_publickey_hash(key.get(),SSH_PUBLICKEY_HASH_SHA256,&actualHash,&hashLength);
        struct HashCleanup {unsigned char*& value;~HashCleanup(){ssh_clean_pubkey_hash(&value);}} cleanup{actualHash};
        if(hashResult!=SSH_OK||!actualHash||hashLength!=admittedHash.size()||
            std::memcmp(actualHash,admittedHash.data(),hashLength)!=0)throw SessionError();
        switch(ssh_key_type(key.get())) {
        case SSH_KEYTYPE_ED25519: case SSH_KEYTYPE_ECDSA_P256: case SSH_KEYTYPE_ECDSA_P384:
        case SSH_KEYTYPE_ECDSA_P521: case SSH_KEYTYPE_RSA: break;
        default: throw SessionError();
        }
        if(!certificate.empty()) {
            if(certificate.size()>65536||certificate.find('\0')!=std::string_view::npos)throw SessionError();
            while(!certificate.empty()&&(certificate.back()=='\r'||certificate.back()=='\n'))certificate.remove_suffix(1);
            if(!validText(certificate,65536))throw SessionError();
            const auto separator=certificate.find(' ');
            if(separator==std::string_view::npos)throw SessionError();
            const std::string algorithm(certificate.substr(0,separator));
            const auto type=ssh_key_type_from_name(algorithm.c_str());
            switch(type) {
            case SSH_KEYTYPE_ED25519_CERT01: case SSH_KEYTYPE_ECDSA_P256_CERT01:
            case SSH_KEYTYPE_ECDSA_P384_CERT01: case SSH_KEYTYPE_ECDSA_P521_CERT01:
            case SSH_KEYTYPE_RSA_CERT01: break;
            default: throw SessionError();
            }
            certificate.remove_prefix(separator+1);
            while(!certificate.empty()&&certificate.front()==' ')certificate.remove_prefix(1);
            const std::string body(certificate.substr(0,certificate.find(' ')));
            ssh_key rawCertificate=nullptr;
            const auto imported=ssh_pki_import_cert_base64(body.c_str(),type,&rawCertificate);
            std::unique_ptr<ssh_key_struct,decltype(&ssh_key_free)> cert(rawCertificate,&ssh_key_free);
            if(imported!=SSH_OK||!cert||ssh_pki_copy_cert_to_privkey(cert.get(),key.get())!=SSH_OK)throw SessionError();
        }
        if(ssh_userauth_publickey(session_,nullptr,key.get())!=SSH_AUTH_SUCCESS)throw SessionError();
        state_=State::Authenticated;
    } catch(...) {state_=State::Failed;throw;}
}
void Session::openShell(int columns,int rows) {
    require(State::Authenticated); dimensions(columns,rows);
    channel_=ssh_channel_new(session_);
    if (!channel_) { state_=State::Failed; throw SessionError(); }
    checked(ssh_channel_open_session(channel_));
    checked(ssh_channel_request_pty_size(channel_,"xterm-256color",columns,rows));
    checked(ssh_channel_request_shell(channel_));
    state_=State::Shell;
}
void Session::resize(int columns,int rows) {
    require(State::Shell); dimensions(columns,rows);
    checked(ssh_channel_change_pty_size(channel_,columns,rows));
}
void Session::write(std::span<const std::byte> bytes) {
    require(State::Shell);
    if(bytes.size()>65536) throw SessionError();
    std::size_t written=0;
    while(written<bytes.size()) {
        const auto result=ssh_channel_write(channel_,bytes.data()+written,static_cast<std::uint32_t>(bytes.size()-written));
        if(result<=0) { state_=State::Failed; throw SessionError(); }
        written+=static_cast<std::size_t>(result);
    }
}
std::vector<std::byte> Session::read(int timeoutMilliseconds) {
    require(State::Shell);
    if(timeoutMilliseconds<0 || timeoutMilliseconds>1000) throw SessionError();
    std::vector<std::byte> result(65536);
    const auto count=ssh_channel_read_timeout(channel_,result.data(),65536,0,timeoutMilliseconds);
    // libssh 0.12.2 documents SSH_AGAIN on timeout, even for this bounded
    // blocking read. An idle shell is not a failed/disconnected session.
    if(count==SSH_AGAIN) return {};
    if(count<0) { state_=State::Failed; throw SessionError(); }
    result.resize(static_cast<std::size_t>(count));
    return result;
}
} // namespace nordssh::ssh
