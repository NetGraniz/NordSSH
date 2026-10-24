#include "vault/VaultService.h"
#include "vault/DeviceKeys.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <future>
#include <iostream>
using namespace nordssh::vault;
static void require(bool value) { if (!value) throw std::runtime_error("Device key contract failed"); }
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    auto task = std::async(std::launch::async, [&] {
        VaultService service(QDir(temp.path()).filePath("synthetic-device"));
        bool badIdentity=false;
        try {(void)DeviceKeys(service.files()).lookup(QString(64,QChar('a'))+'\n');}
        catch(const DeviceError&) {badIdentity=true;}
        require(badIdentity);
        const auto first = service.create("synthetic-password");
        service.rememberDevice(); service.lock();
        require(service.unlockRemembered(first));
        VaultDocument doc; doc.settings.theme = "light"; service.save(doc);
        service.lock(); require(service.unlockRemembered(first)); require(service.document().settings.theme == "light");
        const auto second = service.create("independent-password"); service.lock();
        require(!service.unlockRemembered(second) && !service.unlocked());
        require(service.unlockRemembered(first)); service.forgetDevice(); service.lock();
        require(!service.unlockRemembered(first) && !service.unlocked());
        service.unlock(first, "synthetic-password"); require(service.unlocked());
#ifdef _WIN32
        // Genuine DPAPI ciphertext is identity-bound and authenticated.
        Header header{}; auto bytes = service.files().read(first); std::copy_n(bytes.begin(), HeaderSize, header.begin());
        auto identity = deviceIdentity(service.files().path(first), header);
        service.rememberDevice(); auto blob = service.files().read(identity + ".device"); blob.back() ^= std::byte{1};
        service.files().write(identity + ".device", blob); service.lock();
        bool rejected = false;
        try { (void)service.unlockRemembered(first); } catch (const DeviceError&) { rejected = true; }
        require(rejected && !service.unlocked());
        service.unlock(first, "synthetic-password"); service.forgetDevice();
#endif
    });
    try { task.get(); std::cout << "Actual OS device unlock, reseal, isolation, forget and password fallback passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
