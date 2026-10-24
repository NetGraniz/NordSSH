#include "TransferManager.h"
#include "vault/PrivateFiles.h"
#include <QDirIterator>
#include <QFileInfo>
#include <QDir>
#include <atomic>
#include <set>
#include <utility>

namespace nordssh::ssh {
namespace {
std::atomic<unsigned> workers{0};
class Permit {
public:
    Permit() { if (workers.fetch_add(1) >= 2) { workers.fetch_sub(1); throw TransferError(); } }
    ~Permit() { workers.fetch_sub(1); }
};
void tree(SftpClient& client, const TransferRequest& request, bool upload,
          const std::string& remote, const QString& relative, unsigned depth, unsigned& entries,
          const SftpClient::Progress& progress, OperationControl control) {
    if (depth > 16 || ++entries > 10000 || control.stop.stop_requested() || std::chrono::steady_clock::now() >= control.deadline) throw TransferError();
    auto local = SftpClient::localPath(request.localRoot, relative);
    if (upload) {
        const QFileInfo info(local);
        if (info.isFile()) { client.upload(request.localRoot, relative, remote, request.confirmedReplace, progress, control); return; }
        if (!info.isDir()) throw TransferError();
        // Existing remote directory is admitted by list; errors never become an
        // empty directory or permission to overwrite a file.
        try { (void)client.list(remote, control); }
        catch (const TransferError&) { client.mkdir(remote, true, control); }
        QDirIterator iterator(local, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (iterator.hasNext()) {
            iterator.next(); const auto child = iterator.fileInfo();
            if (child.isSymLink() || !SftpClient::safeName(child.fileName().toUtf8().toStdString())) throw TransferError();
            tree(client, request, true, remote + '/' + child.fileName().toUtf8().toStdString(), relative + '/' + child.fileName(), depth+1, entries, progress, control);
        }
    } else {
        // DownloadTree's root and children are directories; ordinary file nodes
        // are dispatched from the bounded remote directory listing below.
        const auto listing = client.list(remote, control);
        if (!QFileInfo::exists(local)) { vault::PrivateFiles privateDirectory(local); }
        if (!QFileInfo(local).isDir()) throw TransferError();
        std::set<QString> foldedNames;
        for (const auto& child : listing) {
            // Do not collapse remote aliases into one Windows destination.
            if (!foldedNames.insert(QString::fromUtf8(child.name).normalized(QString::NormalizationForm_C).toCaseFolded()).second) throw TransferError();
            const auto childLocal = relative + '/' + QString::fromUtf8(child.name);
            const auto childRemote = remote + '/' + child.name;
            if (child.kind == RemoteEntry::Kind::Directory) tree(client, request, false, childRemote, childLocal, depth+1, entries, progress, control);
            else if (child.kind == RemoteEntry::Kind::File) {
                if (++entries > 10000) throw TransferError();
                client.download(childRemote, request.localRoot, childLocal, request.confirmedReplace, progress, control);
            } else throw TransferError(); // links/special files are reported, never followed
        }
    }
}
}
std::optional<std::uint64_t> TransferManager::enqueue(TransferRequest request) {
    std::lock_guard lock(mutex_);
    if (finishing_ || next_ == 0 || queue_.size() + results_.size() + (state_.id ? 1 : 0) >= MaxJobs ||
        static_cast<unsigned>(request.action) > static_cast<unsigned>(TransferRequest::Action::DownloadTree) ||
        request.remote.empty() || request.remote.size() > 4096 || request.destination.size() > 4096 ||
        request.localRoot.size() > 4096 || request.relative.size() > 4096 ||
        request.timeout < std::chrono::seconds(1) || request.timeout > std::chrono::hours(24)) return {};
    const auto id = next_++; queue_.push_back({id, std::move(request)}); wake_.notify_all(); return id;
}
bool TransferManager::cancel(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    if (!id) return false;
    if (state_.id == id) { jobStop_.request_stop(); wake_.notify_all(); return true; }
    for (auto it = queue_.begin(); it != queue_.end(); ++it) if (it->id == id) {
        results_.push_back({id, TransferResult::Status::Cancelled, {}}); queue_.erase(it); return true;
    }
    return false;
}
void TransferManager::finish() { std::lock_guard lock(mutex_); finishing_ = true; wake_.notify_all(); }
TransferState TransferManager::state() const { std::lock_guard lock(mutex_); auto result = state_; result.queued = queue_.size(); return result; }
std::optional<TransferResult> TransferManager::takeResult() {
    std::lock_guard lock(mutex_); if (results_.empty()) return {};
    auto result = std::move(results_.front()); results_.pop_front(); return result;
}
void TransferManager::cancelPending() {
    finishing_ = true;
    while (!queue_.empty()) { results_.push_back({queue_.front().id, TransferResult::Status::Cancelled, {}}); queue_.pop_front(); }
}
void TransferManager::run(Session& session, std::stop_token stop) {
    {
        std::lock_guard lock(mutex_); if (used_) throw TransferError(); used_ = true;
    }
    try {
        Permit permit;
        SftpClient client(session, {stop, std::chrono::steady_clock::now() + std::chrono::seconds(10)});
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop, [&] { return !queue_.empty() || finishing_; });
                if (stop.stop_requested()) { cancelPending(); break; }
                if (queue_.empty()) break;
                job = std::move(queue_.front()); queue_.pop_front();
                state_ = {job.id, {}, queue_.size()}; jobStop_ = std::stop_source{};
            }
            const auto token = jobStop_.get_token();
            std::stop_callback connectionCancel(stop, [&] { jobStop_.request_stop(); });
            const OperationControl control{token, std::chrono::steady_clock::now() + job.request.timeout};
            const auto progress = [&](TransferProgress value) { std::lock_guard lock(mutex_); state_.progress = value; };
            TransferResult result{job.id, TransferResult::Status::Completed, {}};
            bool lostTransport = false;
            try {
                const auto& r = job.request;
                switch (r.action) {
                case TransferRequest::Action::List: result.entries = client.list(r.remote, control); break;
                case TransferRequest::Action::Mkdir: client.mkdir(r.remote, r.confirmedReplace, control); break;
                case TransferRequest::Action::RemoveFile: client.remove(r.remote,false,r.confirmedReplace,control); break;
                case TransferRequest::Action::RemoveDirectory: client.remove(r.remote,true,r.confirmedReplace,control); break;
                case TransferRequest::Action::Rename: client.rename(r.remote,r.destination,r.confirmedReplace,control); break;
                case TransferRequest::Action::Upload: client.upload(r.localRoot,r.relative,r.remote,r.confirmedReplace,progress,control); break;
                case TransferRequest::Action::Download: client.download(r.remote,r.localRoot,r.relative,r.confirmedReplace,progress,control); break;
                case TransferRequest::Action::UploadTree:
                case TransferRequest::Action::DownloadTree: {
                    unsigned entries = 0;
                    tree(client,r,r.action==TransferRequest::Action::UploadTree,r.remote,r.relative,0,entries,progress,control); break;
                }
                }
            } catch (const TransferCommitUnknown&) { result.status = TransferResult::Status::CommitUnknown; lostTransport = true; }
              catch (const SessionError&) { result.status = token.stop_requested() ? TransferResult::Status::Cancelled : TransferResult::Status::Failed; lostTransport = true; }
              catch (...) { result.status = token.stop_requested() ? TransferResult::Status::Cancelled : TransferResult::Status::Failed; }
            {
                std::lock_guard lock(mutex_); results_.push_back(std::move(result)); state_ = {};
                if (lostTransport || token.stop_requested() || std::chrono::steady_clock::now() >= control.deadline) { cancelPending(); break; }
            }
        }
    } catch (...) { std::lock_guard lock(mutex_); state_ = {}; cancelPending(); throw; }
}
} // namespace nordssh::ssh
