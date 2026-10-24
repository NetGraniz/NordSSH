#pragma once
#include "ConnectionManager.h"
#include "SftpClient.h"
#include <deque>
#include <mutex>
#include <condition_variable>

namespace nordssh::ssh {
struct TransferRequest {
    enum class Action { List, Mkdir, RemoveFile, RemoveDirectory, Rename, Upload, Download, UploadTree, DownloadTree };
    Action action = Action::List;
    std::string remote, destination;
    QString localRoot, relative;
    bool confirmedReplace = false;
    std::chrono::seconds timeout{60}; // complete job, not an indefinitely reset timeout
};
struct TransferResult {
    enum class Status { Completed, Failed, Cancelled, CommitUnknown };
    std::uint64_t id;
    Status status;
    std::vector<RemoteEntry> entries;
};
struct TransferState { std::uint64_t id = 0; TransferProgress progress; std::size_t queued = 0; };
// Thread-safe bounded typed mailbox. One dedicated authenticated ConnectionManager
// service connection per instance. No password copies, Qt signals or GUI joins.
class TransferManager final : public SessionWork {
public:
    static constexpr std::size_t MaxJobs = 16;
    [[nodiscard]] std::optional<std::uint64_t> enqueue(TransferRequest);
    [[nodiscard]] bool cancel(std::uint64_t id);
    void finish(); // reject new requests, drain accepted jobs then close
    [[nodiscard]] TransferState state() const;
    [[nodiscard]] std::optional<TransferResult> takeResult();
    void run(Session&, std::stop_token) override;
private:
    struct Job { std::uint64_t id; TransferRequest request; };
    void cancelPending();
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Job> queue_;
    std::deque<TransferResult> results_;
    TransferState state_;
    std::stop_source jobStop_;
    std::uint64_t next_ = 1;
    bool finishing_ = false, used_ = false;
};
} // namespace nordssh::ssh
