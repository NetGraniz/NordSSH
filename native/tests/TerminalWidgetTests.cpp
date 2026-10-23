#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QImage>
#include <QFontDatabase>
#include <QRawFont>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QTest>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace nordssh;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static QString row(const TerminalFrame& frame, int y) {
    QString text;
    for (int x = 0; x < frame.columns; ++x) text += frame.cells[y * frame.columns + x].text;
    return text;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        TerminalModel model(80, 24);
        model.feed("Hello \xd0"); model.feed("\x96" "e\xcc\x81\xe7\x95\x8c");
        auto frame = model.frame();
        require(row(frame, 0).startsWith(QString::fromUtf8("Hello Жé界")), "Fragmented UTF8/combining renderer cells");
        require(frame.cells[8].width == 2 && frame.cells[9].width == 0, "Wide-cell placement");
        model.feed("\x1b[?1049h\x1b[2J\x1b[H\x1b[38;2;12;34;56m\x1b[48;2;21;43;65m\x1b[1;3;4mA");
        frame = model.frame();
        require(frame.cells[0].text == "A" && frame.cells[0].foreground == QColor(12,34,56) &&
            frame.cells[0].background == QColor(21,43,65) && frame.cells[0].bold && frame.cells[0].italic &&
            frame.cells[0].underline, "Truecolor and SGR frame");
        model.feed("\x1b[?1049l");
        require(row(model.frame(),0).startsWith("Hello"), "Alternate screen restore");
        model.resize(100, 40, 9, 18);
        require(model.frame().columns == 100 && model.frame().rows == 40, "Grid resize");
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        require(model.key(up) == "\x1b[A", "Normal arrow encoding");
        model.feed("\x1b[?1h");
        require(model.key(up) == "\x1bOA", "Application arrow encoding");
        QKeyEvent interrupt(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, "\x03");
        require(model.key(interrupt) == QByteArray(1,'\x03'), "Ctrl+C encoding");
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
        require(model.key(enter) == "\r", "Enter encoding is not suppressed by Qt C0 text");
        (void)model.takeReplies();
        require(model.paste(QString::fromUtf8("echo Привет").toUtf8()), "Safe paste");
        require(model.takeReplies() == QString::fromUtf8("echo Привет").toUtf8(), "No implicit Enter or duplicate");
        require(!model.paste("first\nsecond") && model.takeReplies().isEmpty(), "Unsafe paste writes nothing");
        model.feed("\x1b[?2004h");
        require(model.paste("first\nsecond", true), "Confirmed multiline paste");
        require(model.takeReplies() == "\x1b[200~first\nsecond\x1b[201~", "Bracketed paste mode");
        model.feed("\x1b]52;c;?\x07\x1b]52;c;c2VjcmV0\x07");
        require(model.takeReplies().isEmpty(), "OSC52 has no clipboard callback or response");
        bool rejected = false;
        try { model.feed(QByteArray(65537, 'x')); } catch(const std::invalid_argument&) { rejected = true; }
        require(rejected, "Input batch admission");
        rejected = false;
        try { model.resize(513, 256, 9, 18); } catch(const std::invalid_argument&) { rejected = true; }
        require(rejected, "Grid-size admission");
        bool otherThreadRejected = false;
        std::jthread other([&] {
            try { model.feed("not-owner"); } catch(const std::logic_error&) { otherThreadRejected = true; }
        });
        other.join();
        require(otherThreadRejected, "Terminal ownership before engine access");
        for (int i = 0; i < 20000; ++i) model.feed("bounded output\r\n");
        require(model.residentPageBytes() < 12 * 1024 * 1024, "History resident-page budget, not total RSS");
        TerminalModel flooded;
        QByteArray queries;
        for (int i = 0; i < 10000; ++i) queries += "\x1b[6n";
        rejected = false;
        try { for (int i = 0; i < 10; ++i) flooded.feed(queries); }
        catch(const std::runtime_error&) { rejected = true; }
        require(rejected, "Reply flood bounded and fails closed");

        TerminalWidget widget;
        const auto testFont = qEnvironmentVariable("NORDSSH_TEST_FONT");
        if (!testFont.isEmpty()) {
            const auto id = QFontDatabase::addApplicationFont(testFont);
            require(id >= 0, "Load explicit font for offscreen raster test");
            const auto families = QFontDatabase::applicationFontFamilies(id);
            require(!families.isEmpty(), "Explicit test font has a family");
            widget.setFont(QFont(families.front(), 11));
        }
        const auto rawFont = QRawFont::fromFont(widget.font());
        require(rawFont.isValid() && rawFont.supportsCharacter('A') && rawFont.supportsCharacter(0x041f),
            "Raster test font must contain Latin and Cyrillic, not tofu glyphs");
        QByteArray sent;
        int reads = 0, confirms = 0, failures = 0, resizedColumns = 0;
        widget.setSend([&](const QByteArray& bytes) { sent += bytes; });
        widget.setFailure([&] { ++failures; });
        widget.setResize([&](int columns, int) { resizedColumns = columns; });
        widget.setPasteSource([&] { ++reads; return QString::fromUtf8("echo Привет"); }, [&] { ++confirms; return true; });
        widget.resize(widget.sizeHint()); widget.show(); app.processEvents();
        require(resizedColumns == 80, "Widget grid resize route");
        QTest::keyClick(&widget, Qt::Key_V, Qt::ControlModifier);
        require(reads == 1 && sent == QString::fromUtf8("echo Привет").toUtf8(), "One widget paste route");
#ifdef Q_OS_WIN
        QKeyEvent russianPaste(QEvent::KeyPress, 0x041c, Qt::ControlModifier, 0, 0x56, 0, QString::fromUtf8("м"));
        QApplication::sendEvent(&widget, &russianPaste);
        require(reads == 2, "Russian virtual-key paste once");
#endif
        sent.clear();
        QTest::keyClick(&widget, Qt::Key_C, Qt::ControlModifier);
        require(sent == QByteArray(1, '\x03'), "Widget Ctrl+C remains interrupt");
        sent.clear();
        QTest::keyClick(&widget, Qt::Key_Tab);
        require(sent == "\t", "Tab reaches terminal instead of focus navigation");
        sent.clear();
        QTest::mouseClick(&widget, Qt::RightButton);
        require(sent == QString::fromUtf8("echo Привет").toUtf8(), "Right click single paste");
        sent.clear();
        widget.setPasteSource([&] { ++reads; return QString("one\ntwo"); }, [&] { ++confirms; return false; });
        QTest::keyClick(&widget, Qt::Key_Insert, Qt::ShiftModifier);
        require(sent.isEmpty() && confirms == 1, "Cancelled multiline never sent");
        QInputMethodEvent ime;
        ime.setCommitString(QString::fromUtf8("日本語"));
        QApplication::sendEvent(&widget, &ime);
        require(sent == QString::fromUtf8("日本語").toUtf8(), "IME commit exactly once");
        widget.feed("\x1b[2J\x1b[H\x1b[48;2;21;43;65m\x1b[38;2;12;34;56mA");
        QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent); widget.render(&image);
        require(image.pixelColor(1,1) == QColor(21,43,65), "Native raster background pixel");
        widget.feed("\x1b[0m\x1b[2J\x1b[H\x1b[1;38;2;115;210;150mNordSSH native terminal\x1b[0m\r\n"
            "UTF-8: \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82  e\xcc\x81  \xe7\x95\x8c\r\n"
            "\x1b[31mRed \x1b[32mGreen \x1b[34mBlue \x1b[0mNormal\r\n"
            "\x1b[1mBold\x1b[0m / \x1b[3mItalic\x1b[0m / \x1b[4mUnderline\x1b[0m\r\n"
            "\x1b[38;5;208m256-color\x1b[0m / \x1b[38;2;160;180;250mTruecolor\x1b[0m");
        widget.render(&image);
        require(image.save("terminal-raster.png"), "Synthetic raster evidence saved");
        require(failures == 0 && widget.isEnabled(), "No swallowed widget terminal failure");
        TerminalWidget broken;
        int failureCount = 0;
        broken.setFailure([&] { ++failureCount; });
        broken.feed(QByteArray(65537, 'x'));
        broken.feed("ignored after failure");
        require(!broken.isEnabled() && failureCount == 1, "Widget failure stops input/output exactly once");
        auto* closing = new TerminalWidget;
        QPointer<TerminalWidget> closed(closing);
        closing->setSend([](const QByteArray&) {});
        closing->setPasteSource([] { return QString("one\ntwo"); }, [closing] { delete closing; return true; });
        QKeyEvent closingPaste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
        QApplication::sendEvent(closing, &closingPaste);
        require(!closed, "Nested confirmation may close the terminal safely");
        std::cout << "Qt raster/VT/key/paste engineering gate passed; desktop layout and full SSH UI not asserted\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return EXIT_FAILURE;
    }
}
