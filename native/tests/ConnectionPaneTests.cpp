#include "app/ConnectionPane.h"
#include "app/Theme.h"
#include <QApplication>
#include <QFrame>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>
#include <QThread>
#include <cstdlib>
#include <iostream>

using namespace nordssh;
using namespace nordssh::ssh;
using namespace std::chrono_literals;
static void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
template<class Predicate> void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { QApplication::processEvents(); QThread::msleep(1); }
    require(predicate(), "Native SSH pane deadline exceeded");
}
static QString screen(ConnectionPane& pane) {
    const auto frame = pane.terminal().snapshot();
    QString text;
    for (const auto& cell : frame.cells) text += cell.text;
    return text;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
#ifdef Q_OS_WIN
    // Windows offscreen QPA does not automatically enumerate the native font
    // database. Load installed test fonts, not a fake glyph/text renderer.
    const auto root = qEnvironmentVariable("SystemRoot");
    const auto font = QFontDatabase::addApplicationFont(root + "/Fonts/segoeui.ttf");
    if (font < 0) return EXIT_FAILURE;
    QFontDatabase::addApplicationFont(root + "/Fonts/consola.ttf");
    app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 10));
#endif
    app.setStyleSheet(NativeStyle);
    try {
        require(argc == 4, "Expected loopback pane arguments");
        const auto port = std::stoi(argv[1]); require(port > 1024 && port <= 65535, "Invalid fixture port");
        const std::string mode = argv[3];
        ConnectionRuntime runtime;
        auto pane = std::make_unique<ConnectionPane>(runtime);
        pane->resize(900, 580); pane->show(); QApplication::processEvents();
        auto* auth = pane->findChild<QFrame*>("passwordAuthentication");
        auto* trust = pane->findChild<QFrame*>("hostVerification");
        auto* password = pane->findChild<QLineEdit*>("sshPassword");
        auto* status = pane->findChild<QLabel*>("connectionStatus");
        auto* identity = pane->findChild<QLabel*>("hostFingerprint");
        require(auth && trust && password && status && identity, "Missing native pane controls");
        require(!auth->isVisible() && !trust->isVisible() && !pane->terminal().isEnabled(), "Disconnected pane exposed authentication/input");
        std::optional<TrustedHostPin> saved;
        if (mode == "ui-changed") saved = TrustedHostPin{"localhost", static_cast<std::uint16_t>(port), "ssh-ed25519", {}};
        require(pane->start({"LOCALHOST.", static_cast<std::uint16_t>(port), "fixture-user"}, saved), "Pane start failed");
        if (mode == "ui-changed") {
            await([&] { return pane->connection().status().settled; });
            await([&] { return status->text().contains("changed"); });
            require(!trust->isVisible() && !auth->isVisible() && !pane->terminal().isEnabled(), "Changed host key exposed override/auth");
        } else {
            await([&] { return trust->isVisible(); });
            require(identity->textFormat() == Qt::PlainText && status->textFormat() == Qt::PlainText &&
                identity->text().contains("localhost") && identity->text().contains("SHA256:"), "Host identity not native plaintext");
            require(!auth->isVisible() && !pane->terminal().isEnabled(), "Authentication exposed before trust");
            const auto capture = qEnvironmentVariable("NORDSSH_TEST_CAPTURE_DIR");
            if (!capture.isEmpty() && mode == "ui-password") {
                await([&] { return identity->height() >= identity->heightForWidth(identity->width()); });
                require(pane->grab().save(capture + "/trust.png"), "Trust render failed");
            }
            if (mode == "ui-reject") {
                QTest::mouseClick(pane->findChild<QPushButton*>("rejectHostKey"), Qt::LeftButton);
                await([&] { return pane->connection().status().settled; });
                require(!auth->isVisible(), "Rejection exposed a password prompt");
            } else {
                require(mode == "ui-password" || mode == "ui-eof", "Unknown UI mode");
                QTest::mouseClick(pane->findChild<QPushButton*>("acceptHostKey"), Qt::LeftButton);
                await([&] { return auth->isVisible(); });
                require(password->echoMode() == QLineEdit::Password, "SSH password not masked");
                QTest::keyClick(password, Qt::Key_Return);
                require(pane->findChild<QLabel*>("authenticationError")->isVisible(), "Empty password failed silently");
                password->setText("synthetic-ssh-password");
                QTest::keyClick(password, Qt::Key_Return);
                require(password->text().isEmpty() && !auth->isVisible(), "UI retained submitted password");
                await([&] { return pane->terminal().isEnabled() && screen(*pane).contains("fixture-ready"); });
                pane->terminal().setPasteSource([] { return QString::fromUtf8("once-\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\n"); }, [] { return true; });
                QTest::keyClick(&pane->terminal(), Qt::Key_V, Qt::ControlModifier);
                await([&] { return screen(*pane).contains(QString::fromUtf8("echo:once-\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82")) ||
                    (mode == "ui-eof" && screen(*pane).contains("final-marker")); });
                if (mode == "ui-eof") {
                    await([&] { return screen(*pane).contains("final-marker") && pane->connection().status().settled; });
                    await([&] { return status->text().contains("Server closed"); });
                    require(pane->connection().status().reason == AttemptReason::RemoteClosed, "Final VT query changed EOF outcome");
                }
                if (!capture.isEmpty()) require(pane->grab().save(capture + "/terminal.png"), "Terminal render failed");
                const auto begin = std::chrono::steady_clock::now(); pane.reset();
                require(std::chrono::steady_clock::now() - begin < 100ms, "Live terminal tab close blocked UI");
            }
        }
        pane.reset();
        await([&] { return runtime.idle(); });
        runtime.shutdown();
        std::cout << "Native connection pane " << mode << " passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
