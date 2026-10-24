#include "vault/VaultService.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QLockFile>
#include <QDir>
#include <QFile>
#include <iostream>
#include <future>
#include <cstring>
#include <algorithm>

using namespace nordssh::vault;
static void require(bool value) { if (!value) throw std::runtime_error("Vault persistence contract failed"); }
template<class Action> static void reject(Action action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Unsafe operation admitted");
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp; if (!temp.isValid()) return 1;
    const auto root = QDir(temp.path()).filePath("native-synthetic");
    reject([&] { VaultService wrongThread(root); });
    require(!QFileInfo::exists(root));
    auto task = std::async(std::launch::async, [&] {
        VaultService service(root);
        const auto first = service.create("first-pass");
        const auto initial = service.files().read(first);
        (void)service.document();
        VaultDocument next; next.settings.theme = "light";
        next.groups.push_back({"10000000-0000-4000-8000-000000000001", "Synthetic", {}});
        constexpr std::string_view dummyPassword = "synthetic-saved-secret";
        CredentialRecord credential; credential.id="20000000-0000-4000-8000-000000000001";
        credential.kind="password"; credential.host="localhost"; credential.user="fixture";
        credential.secret=SecretBytes(dummyPassword.size());std::memcpy(credential.secret.bytes().data(),dummyPassword.data(),dummyPassword.size());
        next.credentials.push_back(std::move(credential));
        HostRecord host;host.id="30000000-0000-4000-8000-000000000001";host.name="Fixture";host.host="localhost";host.user="fixture";
        host.group=next.groups[0].id;host.credential=next.credentials[0].id;next.hosts.push_back(std::move(host));
        service.save(next);
        require(service.document().groups.size() == 1);
        require(service.files().read(first + ".previous") == initial);
        const auto saved = service.files().read(first);
        require(saved != initial);
        auto lease = service.lease();
        reject([&] { service.lock(); });
        reject([&] { (void)service.create("different"); });
        lease = VaultLease{};
        reject([&] { service.save(VaultDocument{}, {}, [] { throw std::runtime_error("synthetic pre-rename failure"); }); });
        require(service.files().read(first) == saved && service.document().groups.size() == 1);
        std::stop_source stop; stop.request_stop();
        reject([&] { service.save(VaultDocument{}, stop.get_token()); });
        require(service.files().read(first) == saved);
        QLockFile other(service.files().path("store.lock")); other.setStaleLockTime(0); require(other.tryLock(0));
        reject([&] { service.save(VaultDocument{}); }); other.unlock();
        require(service.files().read(first) == saved);
#ifndef _WIN32
        // Real rename failure, not only an injected exception. Separate store
        // avoids making a QLockFile undeletable during the permission change.
        const auto renameRoot = QDir(temp.path()).filePath("rename-failure");
        PrivateFiles renameFiles(renameRoot); renameFiles.write("sample.nordvault", initial);
        bool renameFailed = false;
        try {
            renameFiles.write("sample.nordvault", saved, [&] {
                require(QFile::setPermissions(renameRoot, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
            });
        } catch (const FileError&) { renameFailed = true; }
        require(QFile::setPermissions(renameRoot, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        require(renameFailed && renameFiles.read("sample.nordvault") == initial);
#endif
        const auto exported = service.exportEncrypted("export-pass");
        auto exportedBytes = service.files().read(exported);
        require(exportedBytes != saved);
        const auto second = service.create("second-pass");
        require(service.document().groups.empty());
        const auto secondInitial = service.files().read(second);
        reject([&] { service.importEncrypted(exportedBytes, "export-pass", false); });
        reject([&] { service.importEncrypted(exportedBytes, "wrong-pass", true); });
        auto corrupt = exportedBytes; corrupt.back() ^= std::byte{1};
        reject([&] { service.importEncrypted(corrupt, "export-pass", true); });
        require(service.files().read(second) == secondInitial);
        service.importEncrypted(exportedBytes, "export-pass", true);
        require(service.document().groups.size() == 1);
        service.lock(); service.unlock(second, "second-pass");
        require(service.document().settings.theme == "light");
        require(service.document().credentials.size()==1&&service.document().hosts.size()==1&&
            std::ranges::equal(service.document().credentials[0].secret.bytes(),std::as_bytes(std::span(dummyPassword))));
        reject([&] { service.unlock(first, "wrong-pass"); });
        require(service.unlocked() && service.document().groups.size() == 1);
        reject([&] { service.unlock("missing.nordvault", "first-pass"); });
        require(service.unlocked());
        // Cooperative stale-write detection: external valid encrypted replacement.
        service.files().write(second, secondInitial);
        reject([&] { service.save(VaultDocument{}); });
        require(service.files().read(second) == secondInitial);
        reject([&] { (void)service.files().path("../escape"); });
        reject([&] { (void)service.files().path("name/escape"); });
        service.unlock(first, "first-pass"); require(service.document().groups.size() == 1);
        service.lock(); reject([&] { (void)service.document(); });
        // Regular file replacing a vault is never silently treated as empty.
        service.files().write("invalid.nordvault", std::as_bytes(std::span("invalid", 7)));
        reject([&] { service.unlock("invalid.nordvault", "first-pass"); });
        require(!service.unlocked());
#ifndef _WIN32
        const auto link = service.files().path("linked.nordvault");
        require(QFile::link(service.files().path(first), link));
        reject([&] { service.unlock("linked.nordvault", "first-pass"); });
#endif
    });
    try { task.get(); std::cout << "Private files, atomic failure/cancel, leases, CAS, lock, independent vaults and encrypted import/export passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
