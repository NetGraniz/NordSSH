#include "ssh/SftpClient.h"
#include "ssh/DirectChannel.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <future>
#include <iostream>
using namespace nordssh::ssh;
using namespace std::chrono_literals;
static void require(bool value) { if (!value) throw std::runtime_error("Real OpenSSH gate failed"); }
static QByteArray read(const QString& path) { QFile file(path); require(file.open(QIODevice::ReadOnly)); return file.readAll(); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc==7);
        const auto port=static_cast<std::uint16_t>(std::stoul(argv[1])),echoPort=static_cast<std::uint16_t>(std::stoul(argv[5]));
        const std::string fingerprint=argv[2],user=argv[3],remote=argv[4],mode=argv[6];
        nordssh::vault::SecretBytes key(16384);
        std::cin.read(reinterpret_cast<char*>(key.bytes().data()),16384);
        const auto length=std::cin.gcount();require(length>0&&length<16384);
        const std::string_view keyText(reinterpret_cast<const char*>(key.bytes().data()),static_cast<std::size_t>(length));
        QTemporaryDir local;require(local.isValid());
        auto worker=std::async(std::launch::async,[&] {
            auto connect=[&](Session& session) {
                session.connectResolved({"openssh.fixture.invalid",port,user},"127.0.0.1");
                const auto identity=session.hostIdentity();
                require(QByteArray(reinterpret_cast<const char*>(identity.sha256.data()),32).toHex().toStdString()==fingerprint);
                if(mode!="untrusted") session.authorizeHost(identity);
                session.authenticatePrivateKey(keyText);
            };
            if(mode=="badkey"||mode=="untrusted") {
                bool refused=false;try {Session session;connect(session);}catch(const SessionError&){refused=true;}
                require(refused);return;
            }
            {
                Session session;connect(session);SftpClient sftp(session);
                require(!sftp.list(remote).empty());
                const auto source=local.path()+"/source";
                {QFile file(source);require(file.open(QIODevice::WriteOnly));require(file.write(QByteArray(327680,'r'))==327680);}
                sftp.upload(local.path(),"source",remote+"/new",false);
                sftp.download(remote+"/new",local.path(),"received",false);
                require(read(local.path()+"/received")==QByteArray(327680,'r'));
                bool refused=false;try{sftp.upload(local.path(),"source",remote+"/existing",false);}catch(const TransferError&){refused=true;}require(refused);
                sftp.download(remote+"/existing",local.path(),"original",false);require(read(local.path()+"/original")=="real-openssh-original");
                sftp.upload(local.path(),"source",remote+"/existing",true);
                sftp.download(remote+"/existing",local.path(),"replaced",false);require(read(local.path()+"/replaced")==QByteArray(327680,'r'));
                sftp.mkdir(remote+"/folder",true);sftp.rename(remote+"/new",remote+"/folder/renamed",true);
                sftp.remove(remote+"/folder/renamed",false,true);sftp.remove(remote+"/folder",true,true);
                refused=false;try{sftp.download(remote+"/linked",local.path(),"escaped",false);}catch(const TransferError&){refused=true;}require(refused);
            }
            {
                Session session;connect(session);DirectChannel channel(session,"127.0.0.1",echoPort,{{},std::chrono::steady_clock::now()+5s});
                const std::string payload="real-openssh-forward";std::size_t sent=0;bool eof=false;std::string output;
                const auto deadline=std::chrono::steady_clock::now()+5s;std::array<std::byte,4096> buffer{};
                while(std::chrono::steady_clock::now()<deadline) {
                    if(sent<payload.size()) sent+=channel.write(std::as_bytes(std::span(payload)).subspan(sent));
                    if(sent==payload.size()&&!eof) eof=channel.sendEof();
                    (void)session.flushSome();
                    const auto n=channel.read(buffer);output.append(reinterpret_cast<const char*>(buffer.data()),n);
                    require(output.size()<4096);if(!n&&channel.eof())break;
                    std::this_thread::sleep_for(1ms);
                }
                require(sent==payload.size()&&eof&&output==payload+"tail-after-eof");
            }
        });
        worker.get();std::cout<<"Real OpenSSH mode passed: "<<mode<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
