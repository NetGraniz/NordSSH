#include "vault/VaultService.h"
#include <QCoreApplication>
#include <QStorageInfo>
#include <QDir>
#include <future>
#include <iostream>
#include <algorithm>
using namespace nordssh::vault;
static void require(bool value) { if (!value) throw std::runtime_error("Actual full-disk vault contract failed"); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc==2);const QString root=QString::fromLocal8Bit(argv[1]);
        const QStorageInfo storage(root);require(storage.isValid()&&storage.bytesTotal()>0&&storage.bytesTotal()<=128*1024);
        auto worker=std::async(std::launch::async,[&] {
            VaultService vault(QDir(root).filePath("vaults"));
            const auto file=vault.create("synthetic-password");const auto original=vault.files().read(file);
            VaultDocument large;
            for(const auto id:{"10000000-0000-4000-8000-000000000001","10000000-0000-4000-8000-000000000002"}) {
                SnippetRecord record;record.id=id;record.name="Synthetic full disk";record.command=SecretBytes(65536);
                std::ranges::fill(record.command.bytes(),std::byte{'x'});large.snippets.push_back(std::move(record));
            }
            bool full=false;try{vault.save(large);}catch(const FileError&){full=true;}
            require(full&&vault.files().read(file)==original&&vault.document().snippets.empty());
            vault.save(VaultDocument{});vault.lock();vault.unlock(file,"synthetic-password");require(vault.document().snippets.empty());
        });
        worker.get();std::cout<<"Actual 128 KiB full-filesystem rollback and subsequent recovery passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
