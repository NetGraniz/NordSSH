#include "Session.h"
#include "KeyMaterial.h"
#include "KeyImportPermit.h"
#include "vault/VaultCrypto.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <openssl/rand.h>
#include <cerrno>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <poll.h>
#endif

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
namespace {
std::string promptText(const char* text, std::size_t maximum) {
    if (!text) return {};
    std::size_t size = 0;
    while (size <= maximum && text[size]) ++size;
    if (size > maximum) throw SessionError();
    const std::string result(text, size);
    if (std::ranges::any_of(result, [](unsigned char c) { return (c < 0x20 && c != '\n' && c != '\t' && c != '\r') || c == 0x7f; }))
        throw SessionError();
    const auto utf8 = vault::validatePassword(result);
    if (utf8 == vault::PasswordStatus::InvalidUtf8 || utf8 == vault::PasswordStatus::TooLong) throw SessionError();
    return result;
}
}
void Session::authenticateInteractive(const InteractiveResponder& respond, OperationControl control) {
    require(State::Trusted);
    if (!respond) throw SessionError();
    try {
        const auto now = std::chrono::steady_clock::now();
        if (control.stop.stop_requested() || now >= control.deadline || control.deadline - now > std::chrono::minutes(5)) failed();
        const auto wire = [&] { return OperationControl{control.stop,
            (std::min)(control.deadline, std::chrono::steady_clock::now() + std::chrono::seconds(10))}; };
        for (unsigned round = 0; round < 8; ++round) {
            const auto result = retry([&] { return ssh_userauth_kbdint(session_, nullptr, nullptr); }, SSH_AUTH_AGAIN, wire());
            if (result == SSH_AUTH_SUCCESS) { state_ = State::Authenticated; return; }
            if (result != SSH_AUTH_INFO) failed();
            const int count = ssh_userauth_kbdint_getnprompts(session_);
            if (count < 0 || count > 8) failed();
            InteractiveChallenge challenge{promptText(ssh_userauth_kbdint_getname(session_), 512),
                promptText(ssh_userauth_kbdint_getinstruction(session_), 2048), {}};
            for (int index = 0; index < count; ++index) {
                char echo = 0;
                const auto* question = ssh_userauth_kbdint_getprompt(session_, static_cast<unsigned>(index), &echo);
                if (!question) failed();
                challenge.questions.push_back({promptText(question, 512), echo != 0});
            }
            auto answers = respond(challenge);
            checkpoint(wire());
            if (answers.size() != challenge.questions.size()) failed();
            for (std::size_t index = 0; index < answers.size(); ++index) {
                const auto bytes = answers[index].bytes();
                if (bytes.size() > 4096 || std::ranges::find(bytes, std::byte{0}) != bytes.end()) failed();
                vault::SecretBytes terminated(bytes.size() + 1);
                std::ranges::copy(bytes, terminated.bytes().begin());
                checked(ssh_userauth_kbdint_setanswer(session_, static_cast<unsigned>(index),
                    reinterpret_cast<const char*>(terminated.bytes().data())));
            }
        }
        failed();
    } catch (...) { state_ = State::Failed; throw; }
}
void Session::keepAlive(OperationControl control) {
    owner();
    if (state_ != State::Shell && state_ != State::Authenticated && state_ != State::Service) throw SessionError();
    checkpoint(control);
    checked(ssh_send_ignore(session_, "NordSSH"));
    checked(retry([&] { return ssh_blocking_flush(session_, 0); }, SSH_AGAIN, control));
}
Session::Session() {
    static const bool initialized = [] { if (ssh_init()!=SSH_OK) throw SessionError(); return true; }();
    (void)initialized;
    if(RAND_bytes(reinterpret_cast<unsigned char*>(connectionId_.data()),16)!=1) throw SessionError();
    session_=ssh_new();
    if (!session_) throw SessionError();
}
Session::~Session() {
    if (session_) ssh_set_blocking(session_, 0);
    if (channel_) ssh_channel_free(channel_);
    if (session_) { ssh_disconnect(session_); ssh_free(session_); }
}
void Session::owner() const { if (owner_!=std::this_thread::get_id()) throw SessionError(); }
void Session::require(State state) const { owner(); if(state_!=state) throw SessionError(); }
void Session::checked(int result) {
    if (result!=SSH_OK) { state_=State::Failed; throw SessionError(); }
}
[[noreturn]] void Session::failed() { state_ = State::Failed; throw SessionError(); }
void Session::checkpoint(const OperationControl& control) {
    const auto now = std::chrono::steady_clock::now();
    if (control.stop.stop_requested() || now >= control.deadline || control.deadline - now > std::chrono::seconds(30)) failed();
}
bool Session::waitReady(const OperationControl& control) {
    if (control.stop.stop_requested()) failed();
    const auto now = std::chrono::steady_clock::now();
    if (now >= control.deadline) return false;
    if (control.deadline - now > std::chrono::seconds(30)) failed();
    const int wait = static_cast<int>(std::clamp<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(control.deadline - now).count(), 1, 25));
    const auto socket = ssh_get_fd(session_);
    if (socket == SSH_INVALID_SOCKET) failed();
    const int flags = ssh_get_poll_flags(session_);
    short events = 0;
    if (flags & SSH_READ_PENDING) events |= POLLIN;
    if (flags & SSH_WRITE_PENDING) events |= POLLOUT;
    if (!events) events = POLLIN;
#ifdef _WIN32
    WSAPOLLFD descriptor{socket, events, 0};
    const int result = WSAPoll(&descriptor, 1, wait);
    if (result == SOCKET_ERROR && WSAGetLastError() != WSAEINTR) failed();
#else
    pollfd descriptor{socket, events, 0};
    const int result = poll(&descriptor, 1, wait);
    if (result < 0 && errno != EINTR) failed();
#endif
    if (control.stop.stop_requested()) failed();
    return true;
}
int Session::retry(const std::function<int()>& operation, int again, const OperationControl& control) {
    while (true) {
        checkpoint(control);
        const int result = operation();
        if (result != again) return result;
        if (!waitReady(control)) failed();
    }
}
void Session::connectResolved(const Endpoint& endpoint, std::string_view address, OperationControl control) {
    require(State::New);
    if (address.empty() || address.size() >= INET6_ADDRSTRLEN || address.find('\0') != std::string_view::npos) failed();
    const std::string numeric(address);
    std::array<unsigned char, 16> storage{};
    if (inet_pton(AF_INET, numeric.c_str(), storage.data()) != 1 &&
        inet_pton(AF_INET6, numeric.c_str(), storage.data()) != 1) failed();
    connectImpl(endpoint, numeric, control);
}
void Session::connectLoopbackTunnel(const Endpoint& endpoint, std::uint16_t localPort, OperationControl control) {
    require(State::New);
    if (!localPort) failed();
    connectImpl(endpoint, "127.0.0.1", control, localPort);
}
void Session::connectImpl(const Endpoint& endpoint, std::string_view address, const OperationControl& control, std::uint16_t transportPort) {
    require(State::New);
    checkpoint(control);
    if (!validText(endpoint.host,253) || !validText(endpoint.user,128) || endpoint.port==0) throw SessionError();
    endpoint_=endpoint;
    const int processConfig=0, logging=SSH_LOG_NOLOG;
    const unsigned int port=transportPort ? transportPort : endpoint.port;
    const long timeout=5;
    checked(ssh_options_set(session_,SSH_OPTIONS_PROCESS_CONFIG,&processConfig));
    checked(ssh_options_set(session_,SSH_OPTIONS_LOG_VERBOSITY,&logging));
    const std::string transportHost(address);
    checked(ssh_options_set(session_,SSH_OPTIONS_HOST,transportHost.c_str()));
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
    ssh_set_blocking(session_, 0);
    checked(retry([&] { return ssh_connect(session_); }, SSH_AGAIN, control));
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
void Session::authenticatePassword(std::string_view password, OperationControl control) {
    require(State::Trusted);
    checkpoint(control);
    if (password.empty() || password.size()>4096 || password.find('\0')!=std::string_view::npos) throw SessionError();
    vault::SecretBytes terminated(password.size()+1);
    std::memcpy(terminated.bytes().data(),password.data(),password.size());
    const auto* text=reinterpret_cast<const char*>(terminated.bytes().data());
    if (retry([&] { return ssh_userauth_password(session_,nullptr,text); }, SSH_AUTH_AGAIN, control) != SSH_AUTH_SUCCESS) failed();
    state_=State::Authenticated;
}
void Session::authenticatePrivateKey(std::string_view privateKey, std::string_view passphrase,
                                    std::string_view certificate, OperationControl control) {
    require(State::Trusted); // Before parsing keys, prompting or transmitting authentication.
    checkpoint(control);
    try {
        std::unique_ptr<ssh_key_struct,decltype(&ssh_key_free)> key(nullptr,&ssh_key_free);
        {
            const KeyImportPermit permit(control);
            const auto admittedHash=admitOpenSshKey(privateKey);
            auto keyText=terminatedSecret(privateKey,1048576),phrase=terminatedSecret(passphrase,4096);
            ssh_key raw=nullptr;
            const auto result=ssh_pki_import_privkey_base64(reinterpret_cast<const char*>(keyText.bytes().data()),
                reinterpret_cast<const char*>(phrase.bytes().data()),&denyImplicitPrompt,nullptr,&raw);
            key.reset(raw);
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
        } // Release CPU admission before waiting for network authentication.
        if(retry([&] { return ssh_userauth_publickey(session_,nullptr,key.get()); }, SSH_AUTH_AGAIN, control)!=SSH_AUTH_SUCCESS)throw SessionError();
        state_=State::Authenticated;
    } catch(...) {state_=State::Failed;throw;}
}
void Session::openShell(int columns,int rows, OperationControl control) {
    require(State::Authenticated); dimensions(columns,rows);
    checkpoint(control);
    channel_=ssh_channel_new(session_);
    if (!channel_) { state_=State::Failed; throw SessionError(); }
    checked(retry([&] { return ssh_channel_open_session(channel_); }, SSH_AGAIN, control));
    checked(retry([&] { return ssh_channel_request_pty_size(channel_,"xterm-256color",columns,rows); }, SSH_AGAIN, control));
    checked(retry([&] { return ssh_channel_request_shell(channel_); }, SSH_AGAIN, control));
    state_=State::Shell;
}
void Session::resize(int columns,int rows, OperationControl control) {
    require(State::Shell); dimensions(columns,rows);
    checked(retry([&] { return ssh_channel_change_pty_size(channel_,columns,rows); }, SSH_AGAIN, control));
}
void Session::write(std::span<const std::byte> bytes, OperationControl control) {
    require(State::Shell);
    if(bytes.size()>65536) throw SessionError();
    std::size_t written=0;
    while(written<bytes.size()) {
        checkpoint(control);
        const auto result=ssh_channel_write(channel_,bytes.data()+written,static_cast<std::uint32_t>(bytes.size()-written));
        if (result == 0 || result == SSH_AGAIN) { if (!waitReady(control)) failed(); continue; }
        if(result<0 || static_cast<std::size_t>(result) > bytes.size() - written) failed();
        written+=static_cast<std::size_t>(result);
    }
    checked(retry([&] { return ssh_blocking_flush(session_, 0); }, SSH_AGAIN, control));
}
bool Session::flushSome(std::stop_token stop) {
    owner();
    if (state_ != State::Shell && state_ != State::Service) throw SessionError();
    if (stop.stop_requested()) failed();
    const auto result = ssh_blocking_flush(session_, 0);
    if (result == SSH_AGAIN) return false;
    checked(result);
    return true;
}
bool Session::transportActive() const {
    owner(); return state_ != State::New && state_ != State::Failed && ssh_is_connected(session_) != 0;
}
std::size_t Session::writeSome(std::span<const std::byte> bytes, std::stop_token stop) {
    require(State::Shell);
    if (stop.stop_requested()) failed();
    if (bytes.size() > 16384) throw SessionError();
    if (bytes.empty()) return 0;
    // Do not append another payload behind a socket backlog. Even a large
    // remote channel window must not turn repeated writes into unbounded memory.
    if (!flushSome(stop)) return 0;
    const auto count = ssh_channel_write(channel_, bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    if (count == 0 || count == SSH_AGAIN) return 0;
    if (count < 0 || static_cast<std::size_t>(count) > bytes.size()) failed();
    return static_cast<std::size_t>(count);
}
std::size_t Session::readSome(std::span<std::byte> destination, bool standardError, std::stop_token stop) {
    require(State::Shell);
    if (stop.stop_requested()) failed();
    if (destination.empty() || destination.size() > 65536) throw SessionError();
    const auto count = ssh_channel_read_nonblocking(channel_, destination.data(),
        static_cast<std::uint32_t>(destination.size()), standardError ? 1 : 0);
    if (count == 0 || count == SSH_AGAIN || count == SSH_EOF) return 0;
    if (count < 0 || static_cast<std::size_t>(count) > destination.size()) failed();
    return static_cast<std::size_t>(count);
}
std::vector<std::byte> Session::read(int timeoutMilliseconds, std::stop_token stop) {
    require(State::Shell);
    if(timeoutMilliseconds<0 || timeoutMilliseconds>1000) throw SessionError();
    std::vector<std::byte> result(65536);
    OperationControl control{stop, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds)};
    while (true) {
        if (stop.stop_requested()) failed();
        const auto count = ssh_channel_read_nonblocking(channel_, result.data(), 65536, 0);
        if (count > 0) { result.resize(static_cast<std::size_t>(count)); return result; }
        if (count != 0 && count != SSH_AGAIN) failed();
        if (closed() || !waitReady(control)) return {};
    }
}
bool Session::closed() const {
    require(State::Shell);
    return ssh_is_connected(session_) == 0 || ssh_channel_is_eof(channel_) != 0;
}
} // namespace nordssh::ssh
