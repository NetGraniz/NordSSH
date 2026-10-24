#pragma once
#include <QString>
#include "Session.h"
#include <libssh/sftp.h>

namespace nordssh::ssh {
class TransferError final : public std::runtime_error {
public: TransferError() : std::runtime_error("SFTP operation failed, unsafe path or overwrite not confirmed") {}
};
struct RemoteEntry {
    enum class Kind { File, Directory, Link, Other };
    std::string name;
    Kind kind = Kind::Other;
    std::uint64_t size = 0, modified = 0;
};
class TransferCommitUnknown final : public std::runtime_error {
public: TransferCommitUnknown() : std::runtime_error("Remote commit acknowledgement lost; refresh the destination before retrying") {}
};
struct TransferProgress { std::uint64_t done = 0, total = 0; };
// Exclusive authenticated Session on a dedicated worker, never a terminal's
// live libssh session. All paths are absolute, symlink ancestors are refused.
class SftpClient final {
public:
    SftpClient(Session&, OperationControl = {});
    ~SftpClient();
    SftpClient(const SftpClient&) = delete;
    SftpClient& operator=(const SftpClient&) = delete;
    [[nodiscard]] std::vector<RemoteEntry> list(const std::string&, OperationControl = {});
    void mkdir(const std::string&, bool confirmed, OperationControl = {});
    void remove(const std::string&, bool directory, bool confirmed, OperationControl = {});
    void rename(const std::string&, const std::string&, bool confirmedReplace, OperationControl = {});
    using Progress = std::function<void(TransferProgress)>; // worker mailbox sink only
    void download(const std::string& remote, const QString& localRoot, const QString& relative,
                  bool confirmedReplace, Progress = {}, OperationControl = {});
    void upload(const QString& localRoot, const QString& relative, const std::string& remote,
                bool confirmedReplace, Progress = {}, OperationControl = {});
    [[nodiscard]] static bool safeName(std::string_view);
    [[nodiscard]] static QString localPath(const QString& root, const QString& relative);
private:
    Session& session_;
    sftp_session sftp_{};
    [[nodiscard]] std::optional<RemoteEntry> inspect(const std::string& path, bool missing = false);
    void path(const std::string& path, bool leafMayBeMissing = false);
};
} // namespace nordssh::ssh
