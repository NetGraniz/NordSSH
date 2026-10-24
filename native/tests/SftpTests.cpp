#include "ssh/TransferManager.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <future>
#include <iostream>
#include <cstring>
using namespace nordssh::ssh;
using namespace std::chrono_literals;
static void require(bool value) { if (!value) throw std::runtime_error("SFTP contract failed"); }
template<class Action> static void reject(Action action) { try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Unsafe SFTP operation accepted"); }
static void write(const QString& path, const QByteArray& content) { QFile file(path); require(file.open(QIODevice::WriteOnly)); require(file.write(content) == content.size()); }
static QByteArray read(const QString& path) { QFile file(path); require(file.open(QIODevice::ReadOnly)); return file.readAll(); }
static void connect(Session& session, unsigned port, const std::string& hash) {
    session.connectResolved({"localhost", static_cast<std::uint16_t>(port), "fixture-user"}, "127.0.0.1");
    const auto identity = session.hostIdentity();
    const auto actual = QByteArray(reinterpret_cast<const char*>(identity.sha256.data()), 32).toHex().toStdString();
    require(actual == hash); session.authorizeHost(identity); session.authenticatePassword("synthetic-ssh-password");
}
static void queue(unsigned port, const std::string& hash, const QString& root) {
    auto transfers = std::make_shared<TransferManager>();
    require(QDir(root).mkdir("source-tree")); require(QDir(root + "/source-tree").mkdir("nested"));
    write(root + "/source-tree/a.txt", "tree-a"); write(root + "/source-tree/nested/b.bin", QByteArray(131072, 'b'));
    TransferRequest upload; upload.action = TransferRequest::Action::UploadTree; upload.remote = "/tree"; upload.localRoot = root; upload.relative = "source-tree";
    require(transfers->enqueue(std::move(upload)).has_value());
    TransferRequest download; download.action = TransferRequest::Action::DownloadTree; download.remote = "/tree"; download.localRoot = root; download.relative = "received-tree";
    require(transfers->enqueue(std::move(download)).has_value());
    std::uint64_t cancelled = 0;
    for (unsigned i = 2; i < TransferManager::MaxJobs; ++i) { TransferRequest r; r.remote = "/"; const auto id = transfers->enqueue(r); require(id.has_value()); cancelled = *id; }
    TransferRequest overflow; overflow.remote = "/"; require(!transfers->enqueue(overflow)); require(transfers->cancel(cancelled)); transfers->finish(); require(!transfers->enqueue(overflow));
    ConnectionRuntime runtime; ConnectionManager manager(runtime);
    require(manager.start({"localhost", static_cast<std::uint16_t>(port), "fixture-user"}, {}, {}, 80,24,transfers));
    QElapsedTimer timer; timer.start(); bool approved = false, authenticated = false;
    while (timer.elapsed() < 20000) {
        QCoreApplication::processEvents();
        if (auto prompt = manager.hostPrompt(); prompt && !approved) {
            require(QByteArray(reinterpret_cast<const char*>(prompt->identity.sha256.data()), 32).toHex().toStdString() == hash);
            require(manager.approve(prompt->attempt,prompt->challenge)); approved = true;
        }
        const auto status = manager.status();
        if (status.phase == AttemptPhase::Authentication && !authenticated) {
            constexpr std::string_view password = "synthetic-ssh-password";
            nordssh::vault::SecretBytes bytes(password.size()); std::memcpy(bytes.bytes().data(), password.data(), password.size());
            require(manager.authenticate(*status.request, {Credentials::Kind::Password, std::move(bytes),nordssh::vault::SecretBytes(0),{}})); authenticated = true;
        }
        if (status.settled) { require(status.phase == AttemptPhase::Closed); break; }
        QThread::msleep(2);
    }
    require(manager.status().settled && approved && authenticated);
    unsigned complete = 0, cancelledCount = 0;
    while (auto result = transfers->takeResult()) {
        if (result->status == TransferResult::Status::Completed) ++complete;
        if (result->status == TransferResult::Status::Cancelled) ++cancelledCount;
        require(result->status != TransferResult::Status::Failed);
    }
    require(complete == 15 && cancelledCount == 1);
    require(read(root + "/received-tree/a.txt") == "tree-a" && read(root + "/received-tree/nested/b.bin") == QByteArray(131072,'b'));
    runtime.shutdown(); while (!runtime.idle()) { QCoreApplication::processEvents(); QThread::msleep(1); }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc == 4); const auto port = static_cast<unsigned>(std::stoul(argv[1])); const std::string hash = argv[2], mode = argv[3];
        QTemporaryDir temp; require(temp.isValid()); const auto root = temp.path();
        if (mode == "queue") { queue(port,hash,root); return 0; }
        auto worker = std::async(std::launch::async,[&] {
            for (const auto name : {"../bad","a/b","a\\b","C:bad","NUL","CON.txt","LPT1","tail.","tail "}) require(!SftpClient::safeName(name));
            require(SftpClient::safeName("тест.txt"));
            Session session; connect(session,port,hash); SftpClient client(session);
            if (mode == "timeout" || mode == "cancel-download") {
                write(root + "/target", "local-original"); std::stop_source source;
                std::jthread cancel;
                if (mode == "cancel-download") cancel = std::jthread([&] { std::this_thread::sleep_for(100ms); source.request_stop(); });
                const auto start = std::chrono::steady_clock::now();
                reject([&] { client.download("/stall",root,"target",true,{}, {source.get_token(),start + (mode == "timeout" ? 200ms : 5s)}); });
                require(std::chrono::steady_clock::now() - start < 3s); require(read(root + "/target") == "local-original"); return;
            }
            write(root + "/source", QByteArray(262144,'u'));
            if (mode == "cancel-upload") {
                std::stop_source stop;
                reject([&] { client.upload(root,"source","/existing",true,[&](TransferProgress) {stop.request_stop();}, {stop.get_token(),std::chrono::steady_clock::now()+5s}); }); return;
            }
            if (mode == "no-extension") { reject([&] { client.upload(root,"source","/existing",true); }); return; }
            if (mode == "commit-unknown") {
                bool unknown = false;
                try { client.upload(root,"source","/existing",true); }
                catch (const TransferCommitUnknown&) { unknown = true; }
                require(unknown); return;
            }
            if (mode == "oversize") {
                reject([&] { client.download("/existing",root,"oversize",false); });
                require(!QFileInfo::exists(root + "/oversize")); return;
            }
            require(!client.list("/").empty());
            reject([&] { client.upload(root,"source","/existing",false); });
            reject([&] { client.download("/existing",root,"../escape",false); });
            reject([&] { (void)client.list("/../escape"); });
            reject([&] { client.mkdir("/created",false); }); client.mkdir("/created",true);
            client.upload(root,"source","/created/new",false);
            client.download("/created/new",root,"received",false);
            require(read(root + "/received") == QByteArray(262144,'u'));
            reject([&] { client.download("/existing",root,"received",false); });
            client.download("/existing",root,"received",true); require(read(root + "/received") == "remote-original");
            client.upload(root,"source","/existing",true);
            client.rename("/created/new","/created/renamed",true);
            reject([&] { client.remove("/created/renamed",false,false); });
            client.remove("/created/renamed",false,true); client.remove("/created",true,true);
        });
        worker.get(); std::cout << "SFTP fixture mode passed: " << mode << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
