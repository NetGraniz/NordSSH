#include "Session.h"
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
