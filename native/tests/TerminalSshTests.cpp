// Test orchestration only. The product still needs a cancellable connection
// manager, persistent trust and its own native authentication dialogs.
#include "terminal/TerminalWidget.h"
#include "ssh/Session.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string fingerprint(const std::array<std::byte,32>& hash) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : hash) {
        auto value = std::to_integer<unsigned>(byte);
        result += hex[value >> 4]; result += hex[value & 15];
    }
    return result;
}
struct Mailbox {
    std::mutex mutex;
    QByteArray received, outgoing;
    bool resize = false;
    std::atomic_bool ready{false}, failed{false};
};
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        require(argc == 4 && std::string(argv[3]) == "terminal", "Isolated fixture arguments required");
        const int port = std::stoi(argv[1]);
        require(port > 1024 && port <= 65535, "Loopback test port out of range");
        const std::string pinned = argv[2];
        Mailbox mailbox;
        nordssh::TerminalWidget widget;
        widget.resize(widget.sizeHint());
        widget.setFailure([&] { mailbox.failed = true; });
        widget.setSend([&](const QByteArray& bytes) {
            std::lock_guard lock(mailbox.mutex);
            require(mailbox.outgoing.size() + bytes.size() <= 65536, "Fixture input queue exceeded limit");
            mailbox.outgoing += bytes;
        });
        widget.setResize([&](int columns, int rows) {
            if (columns == 100 && rows == 40) {
                std::lock_guard lock(mailbox.mutex); mailbox.resize = true;
            }
        });
        widget.setPasteSource([] { return QString::fromUtf8("once-привет"); }, [] { return false; });
        widget.show();
        std::jthread worker([&](std::stop_token stop) {
            const char* stage = "connect";
            try {
                nordssh::ssh::Session client;
                client.connectResolved({"127.0.0.1", static_cast<std::uint16_t>(port), "fixture-user"}, "127.0.0.1");
                const auto identity = client.hostIdentity();
                stage = "host approval";
                require(fingerprint(identity.sha256) == pinned, "Fixture host key mismatch");
                client.authorizeHost(identity);
                stage = "authentication";
                client.authenticatePassword("synthetic-ssh-password");
                stage = "PTY open";
                client.openShell(80,24);
                stage = "PTY transfer";
                mailbox.ready = true;
                while (!stop.stop_requested()) {
                    QByteArray outgoing;
                    bool resize = false;
                    {
                        std::lock_guard lock(mailbox.mutex);
                        outgoing.swap(mailbox.outgoing); resize = std::exchange(mailbox.resize, false);
                    }
                    if (resize) client.resize(100,40);
                    if (!outgoing.isEmpty()) client.write(std::as_bytes(std::span(outgoing.constData(),
                        static_cast<std::size_t>(outgoing.size()))));
                    const auto bytes = client.read(25);
                    if (!bytes.empty()) {
                        std::lock_guard lock(mailbox.mutex);
                        require(mailbox.received.size() + static_cast<qsizetype>(bytes.size()) <= 262144,
                            "Fixture output queue exceeded limit");
                        mailbox.received.append(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<qsizetype>(bytes.size()));
                    }
                }
            } catch (const std::exception& error) {
                std::cerr << "Fixture stage: " << stage << ": " << error.what() << '\n';
                mailbox.failed = true;
            }
        });
        QTimer tick;
        bool pasted = false, success = false;
        const auto started = std::chrono::steady_clock::now();
        QObject::connect(&tick, &QTimer::timeout, &app, [&] {
            if (mailbox.failed || std::chrono::steady_clock::now() - started > std::chrono::seconds(8)) {
                app.exit(EXIT_FAILURE); return;
            }
            QByteArray bytes;
            { std::lock_guard lock(mailbox.mutex); bytes.swap(mailbox.received); }
            if (!bytes.isEmpty()) widget.feed(bytes);
            auto frame = widget.snapshot();
            QString visible;
            for (const auto& cell : frame.cells) visible += cell.text;
            if (!pasted && mailbox.ready && visible.contains("fixture-ready")) {
                pasted = true;
                const QSize initial = widget.sizeHint();
                widget.resize(initial.width() / 80 * 100, initial.height() / 24 * 40);
                QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
                QApplication::sendEvent(&widget, &paste);
                for (const auto type : {QEvent::MouseButtonPress,QEvent::MouseButtonRelease}) {
                    QMouseEvent click(type,{5,5},{5,5},Qt::LeftButton,
                        type==QEvent::MouseButtonPress?Qt::LeftButton:Qt::NoButton,Qt::NoModifier);
                    QApplication::sendEvent(&widget,&click);
                }
                QWheelEvent wheel({5,5},{5,5},{},{0,120},Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
                QApplication::sendEvent(&widget,&wheel);
            }
            if (visible.contains(QString::fromUtf8("echo:once-привет"))) {
                success = true; app.exit(EXIT_SUCCESS);
            }
        });
        tick.start(10);
        const int result = app.exec();
        worker.request_stop(); worker.join();
        require(success && result == EXIT_SUCCESS && !mailbox.failed, "Real SSH/render/paste gate failed");
        std::cout << "Qt terminal real loopback SSH UTF8 single-paste/mouse/resize passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
