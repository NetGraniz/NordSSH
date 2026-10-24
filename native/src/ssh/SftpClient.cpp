#include "SftpClient.h"
#include "TransportDeadline.h"
#include "vault/PrivateFiles.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDateTime>
#include <algorithm>
#include <set>
#include <memory>
#include <fcntl.h>
#include <openssl/rand.h>

namespace nordssh::ssh {
namespace {
[[noreturn]] void bad() { throw TransferError(); }
void checked(int code) { if (code != SSH_OK) bad(); }
using Attributes = std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)>;
using File = std::unique_ptr<sftp_file_struct, decltype(&sftp_close)>;
using Directory = std::unique_ptr<sftp_dir_struct, decltype(&sftp_closedir)>;
RemoteEntry entry(const sftp_attributes_struct& a, std::string name = {}) {
    RemoteEntry::Kind kind = RemoteEntry::Kind::Other;
    if (a.type == SSH_FILEXFER_TYPE_REGULAR) kind = RemoteEntry::Kind::File;
    if (a.type == SSH_FILEXFER_TYPE_DIRECTORY) kind = RemoteEntry::Kind::Directory;
    if (a.type == SSH_FILEXFER_TYPE_SYMLINK) kind = RemoteEntry::Kind::Link;
    return {std::move(name), kind, a.size, a.mtime};
}
bool same(const std::optional<RemoteEntry>& a, const std::optional<RemoteEntry>& b) {
    return a.has_value() == b.has_value() && (!a || (a->kind == b->kind && a->size == b->size && a->modified == b->modified));
}
std::string randomPart(const std::string& target) {
    std::array<unsigned char,16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) bad();
    return target.substr(0, target.find_last_of('/') + 1) + ".nordssh-" +
        QByteArray(reinterpret_cast<const char*>(random.data()), static_cast<qsizetype>(random.size())).toHex().toStdString() + ".part";
}
constexpr std::uint64_t MaxTransferSize = 1ULL << 40;
}
bool SftpClient::safeName(std::string_view name) {
    if (name.empty() || name.size() > 255 || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') return false;
    if (std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos; })) return false;
    if (vault::validatePassword(name) == vault::PasswordStatus::InvalidUtf8) return false;
    const auto base = QString::fromUtf8(name).section('.', 0, 0).toUpper();
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$" ||
        (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT")) &&
         ((base[3] >= QChar('0') && base[3] <= QChar('9')) || base[3] == QChar(0x00b9) || base[3] == QChar(0x00b2) || base[3] == QChar(0x00b3)))) return false;
    return true;
}
QString SftpClient::localPath(const QString& root, const QString& relative) {
    if (root.contains(QChar(0)) || relative.isEmpty() || relative.size() > 4096 || QDir::isAbsolutePath(relative) || relative.contains('\\')) bad();
    vault::rejectLocalLinks(root);
    if (!QFileInfo(root).isDir()) bad();
    auto result = QFileInfo(root).absoluteFilePath();
    const auto parts = relative.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (!safeName(parts[i].toUtf8().toStdString())) bad();
        result = QDir(result).filePath(parts[i]);
        const QFileInfo info(result);
        if (info.isSymLink()) bad();
        if (info.exists()) vault::rejectLocalLinks(result);
        else if (i + 1 != parts.size()) bad();
        if (i + 1 != parts.size() && !info.isDir()) bad();
    }
    return result;
}
SftpClient::SftpClient(Session& session, OperationControl control) : session_(session) {
    session_.require(Session::State::Authenticated);
    TransportDeadline deadline(session_, control);
    ssh_set_blocking(session_.session_, 1);
    session_.state_ = Session::State::Service;
    try {
        sftp_ = sftp_new(session_.session_);
        if (!sftp_ || sftp_init(sftp_) != SSH_OK) bad();
        deadline.check();
    } catch (...) {
        if (sftp_) { sftp_free(sftp_); sftp_ = nullptr; }
        ssh_set_blocking(session_.session_, 0);
        session_.state_ = Session::State::Failed; throw;
    }
}
SftpClient::~SftpClient() {
    if (sftp_) {
        try {
            // Even protocol teardown has a bounded socket lifetime.
            if (session_.state_ != Session::State::Failed) {
                TransportDeadline deadline(session_, {{}, std::chrono::steady_clock::now() + std::chrono::seconds(2)});
                sftp_free(sftp_); sftp_ = nullptr;
            }
        } catch (...) {}
        if (sftp_) sftp_free(sftp_); // failed/cancelled socket already shut down
    }
    session_.state_ = Session::State::Failed; // exclusive service transport cannot be reused as a shell
    ssh_set_blocking(session_.session_, 0);
}
std::optional<RemoteEntry> SftpClient::inspect(const std::string& name, bool missing) {
    Attributes a(sftp_lstat(sftp_, name.c_str()), &sftp_attributes_free);
    if (!a) { if (missing && sftp_get_error(sftp_) == SSH_FX_NO_SUCH_FILE) return {}; bad(); }
    return entry(*a);
}
void SftpClient::path(const std::string& name, bool missing) {
    session_.require(Session::State::Service);
    if (name.empty() || name.size() > 4096 || name[0] != '/' || (name.size() > 1 && name.back() == '/')) bad();
    if (name == "/") return;
    std::size_t at = 1;
    while (at < name.size()) {
        auto end = name.find('/', at); if (end == std::string::npos) end = name.size();
        if (!safeName(std::string_view(name).substr(at, end-at))) bad();
        const auto component = inspect(name.substr(0,end), missing && end == name.size());
        if (component && (component->kind == RemoteEntry::Kind::Link ||
            (end != name.size() && component->kind != RemoteEntry::Kind::Directory))) bad();
        at = end + 1;
    }
}
std::vector<RemoteEntry> SftpClient::list(const std::string& name, OperationControl control) {
    TransportDeadline deadline(session_, control); path(name);
    Directory dir(sftp_opendir(sftp_, name.c_str()), &sftp_closedir); if (!dir) bad();
    std::vector<RemoteEntry> result; std::set<std::string> names;
    for (unsigned count = 0; ; ++count) {
        deadline.check(); if (count > 10002) bad();
        Attributes a(sftp_readdir(sftp_, dir.get()), &sftp_attributes_free);
        if (!a) { if (!sftp_dir_eof(dir.get())) bad(); break; }
        if (!a->name) bad();
        std::size_t size = 0; while (size <= 255 && a->name[size]) ++size;
        const std::string child(a->name, size);
        if (child == "." || child == "..") continue;
        if (!safeName(child) || !names.insert(child).second || result.size() == 10000) bad();
        result.push_back(entry(*a, child));
    }
    deadline.check(); return result;
}
void SftpClient::mkdir(const std::string& name, bool confirmed, OperationControl control) {
    if (!confirmed || name == "/") bad();
    TransportDeadline deadline(session_, control); path(name, true);
    checked(sftp_mkdir(sftp_, name.c_str(), 0700)); deadline.check();
}
void SftpClient::remove(const std::string& name, bool directory, bool confirmed, OperationControl control) {
    if (!confirmed || name == "/") bad();
    TransportDeadline deadline(session_, control); path(name);
    const auto before = inspect(name);
    if ((directory && before->kind != RemoteEntry::Kind::Directory) || (!directory && before->kind != RemoteEntry::Kind::File)) bad();
    checked(directory ? sftp_rmdir(sftp_, name.c_str()) : sftp_unlink(sftp_, name.c_str())); deadline.check();
}
void SftpClient::rename(const std::string& from, const std::string& to, bool confirmed, OperationControl control) {
    if (!confirmed || from == "/" || to == "/" || from == to) bad();
    TransportDeadline deadline(session_, control); path(from); path(to, true);
    if (!sftp_extension_supported(sftp_, "posix-rename@openssh.com", "1")) bad();
    checked(sftp_rename(sftp_, from.c_str(), to.c_str())); deadline.check();
}
void SftpClient::download(const std::string& remote, const QString& root, const QString& relative,
                          bool confirmed, Progress progress, OperationControl control) {
    TransportDeadline deadline(session_, control); path(remote);
    const auto before = inspect(remote);
    if (before->kind != RemoteEntry::Kind::File || before->size > MaxTransferSize) bad();
    const auto target = localPath(root, relative);
    const QFileInfo initial(target);
    if (initial.exists() && (!confirmed || !initial.isFile())) bad();
    File input(sftp_open(sftp_, remote.c_str(), O_RDONLY, 0), &sftp_close); if (!input) bad();
    QSaveFile output(target); output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) bad();
    vault::protectLocalHandle(output.handle());
#ifndef _WIN32
    if (!output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) bad();
#endif
    vault::SecretBytes buffer(65536); std::uint64_t done = 0;
    for (;;) {
        deadline.check(); const auto amount = sftp_read(input.get(), buffer.bytes().data(), buffer.bytes().size());
        if (amount < 0) bad();
        if (!amount) break;
        const auto count = static_cast<std::uint64_t>(amount);
        if (count > before->size - done || output.write(reinterpret_cast<const char*>(buffer.bytes().data()), amount) != amount) bad();
        done += count; if (progress) progress({done, before->size});
    }
    if (done != before->size || !same(before, inspect(remote))) bad();
    const QFileInfo current(localPath(root, relative));
    if (current.exists() != initial.exists() || (initial.exists() &&
        (current.size() != initial.size() || current.lastModified() != initial.lastModified()))) bad();
    deadline.check(); if (!output.commit()) bad();
}
void SftpClient::upload(const QString& root, const QString& relative, const std::string& remote,
                        bool confirmed, Progress progress, OperationControl control) {
    TransportDeadline deadline(session_, control); path(remote, true);
    const auto before = inspect(remote, true);
    if (before && (!confirmed || before->kind != RemoteEntry::Kind::File)) bad();
    // libssh 0.12.2 sftp_rename uses POSIX overwrite automatically. For a NEW
    // target use exclusive hardlink creation instead; never overwrite a race.
    if (!sftp_extension_supported(sftp_, before ? "posix-rename@openssh.com" : "hardlink@openssh.com", "1")) bad();
    const auto source = localPath(root, relative); const QFileInfo initial(source);
    if (!initial.isFile() || initial.size() < 0 || static_cast<std::uint64_t>(initial.size()) > MaxTransferSize) bad();
    QFile input(source); if (!input.open(QIODevice::ReadOnly)) bad();
    const auto staged = randomPart(remote);
    File output(sftp_open(sftp_, staged.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600), &sftp_close); if (!output) bad();
    bool committed = false;
    try {
        vault::SecretBytes buffer(65536); std::uint64_t done = 0, total = static_cast<std::uint64_t>(initial.size());
        for (;;) {
            deadline.check(); const auto amount = input.read(reinterpret_cast<char*>(buffer.bytes().data()), 65536);
            if (amount < 0) bad();
            if (!amount) break;
            const auto count = static_cast<std::uint64_t>(amount);
            if (count > total - done) bad();
            std::size_t sent = 0;
            while (sent < static_cast<std::size_t>(amount)) {
                deadline.check(); const auto written = sftp_write(output.get(), buffer.bytes().data() + sent, static_cast<std::size_t>(amount) - sent);
                if (written <= 0) bad();
                sent += static_cast<std::size_t>(written);
            }
            done += count; if (progress) progress({done,total});
        }
        const QFileInfo current(localPath(root, relative));
        if (done != total || current.size() != initial.size() || current.lastModified() != initial.lastModified() || !same(before, inspect(remote,true))) bad();
        if (sftp_extension_supported(sftp_, "fsync@openssh.com", "1")) checked(sftp_fsync(output.get()));
        checked(sftp_close(output.release())); deadline.check();
        const auto result = before ? sftp_rename(sftp_, staged.c_str(), remote.c_str()) : sftp_hardlink(sftp_, staged.c_str(), remote.c_str());
        // A failed acknowledgement cannot prove whether commit happened.
        // Preserve staging evidence and require a refresh, never auto-retry.
        if (result != SSH_OK) throw TransferCommitUnknown();
        committed = true;
        if (!before) (void)sftp_unlink(sftp_, staged.c_str());
    } catch (const TransferCommitUnknown&) { output.reset(); throw;
    } catch (...) {
        output.reset();
        if (!control.stop.stop_requested() && std::chrono::steady_clock::now() < control.deadline && !committed) (void)sftp_unlink(sftp_, staged.c_str());
        // On socket cancellation an identifiable staging file can remain. No
        // original target is deleted as cleanup or an overwrite fallback.
        throw;
    }
}
} // namespace nordssh::ssh
