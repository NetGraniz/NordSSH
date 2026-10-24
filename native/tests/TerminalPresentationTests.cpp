#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QImage>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace nordssh;
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static QImage raster(TerminalWidget& widget) {
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent); widget.render(&image); return image;
}
static void ime(TerminalWidget& widget, const QString& preedit, const QString& commit = {}, int replacement = 0) {
    QInputMethodEvent event(preedit, {}); event.setCommitString(commit, replacement);
    QApplication::sendEvent(&widget, &event);
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        TerminalModel model;
        for (int style = 1; style <= 6; ++style) {
            model.feed("\x1b[" + QByteArray::number(style) + " q");
            const auto frame = model.frame();
            require(frame.cursorBlink == (style % 2 == 1), "DECSCUSR blink mapping");
            require(frame.cursorShape == (style < 3 ? TerminalCursorShape::Block :
                style < 5 ? TerminalCursorShape::Underline : TerminalCursorShape::Bar), "DECSCUSR shape mapping");
        }
        model.feed("\x1b]12;#123456\x07");
        require(model.frame().cursorColor == QColor(0x12, 0x34, 0x56), "Effective cursor color");
        model.feed("\x1b[?25l"); require(!model.frame().cursorVisible, "Hidden cursor mode");
        model.feed("\x1b[?25h"); require(model.frame().cursorVisible, "Visible cursor mode");

        TerminalWidget widget;
#ifdef Q_OS_WIN
        const QFont primary("Consolas", 11);
#else
        const QFont primary("DejaVu Sans Mono", 11);
#endif
        widget.setFont(primary); widget.resize(widget.sizeHint()); widget.show();
        widget.activateWindow(); widget.setFocus(); app.processEvents();
        require(widget.hasFocus(), "Offscreen focus fixture");
        QByteArray sent; int failures = 0;
        widget.setSend([&](const QByteArray& bytes) { sent += bytes; });
        widget.setFailure([&] { ++failures; });
        widget.feed("\x1b[2 q\x1b[2J\x1b[H");
        auto frame = widget.snapshot();
        auto block = raster(widget);
        require(block.pixelColor(2, 2) == frame.cursorColor, "Filled block cursor raster");
        widget.feed("\x1b[6 q"); auto bar = raster(widget);
        require(bar.pixelColor(0, 2) == frame.cursorColor && bar.pixelColor(3, 2) == frame.background, "Bar cursor raster");
        widget.feed("\x1b[4 q"); auto underline = raster(widget);
        const int height = widget.sizeHint().height() / 24;
        require(underline.pixelColor(3, height - 1) == frame.cursorColor && underline.pixelColor(3, 2) == frame.background,
            "Underline cursor raster");
        widget.feed("\x1b[1 q"); auto* timer = widget.findChild<QTimer*>("terminalCursorTimer");
        require(timer && timer->isActive(), "Focused blinking cursor has timer");
        const auto beforeBlink = raster(widget);
        QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        require(raster(widget) != beforeBlink, "Deterministic blink phase changes raster");
        widget.hide(); require(!timer->isActive(), "Hidden terminal does not blink");
        widget.show(); widget.setFocus(); app.processEvents(); widget.feed("\x1b[2 q");
        require(!timer->isActive(), "Steady cursor does not run blink timer");

        const auto beforePreedit = raster(widget);
        ime(widget, QString::fromUtf8("日本語"));
        require(sent.isEmpty() && raster(widget) != beforePreedit, "Composition visible locally without network writes");
        QInputMethodQueryEvent composingCursor(Qt::ImCursorRectangle);
        QApplication::sendEvent(&widget, &composingCursor);
        require(composingCursor.value(Qt::ImCursorRectangle).toRect().x() > 0,
            "Input-method candidate anchor follows local composition caret");
        ime(widget, {});
        require(sent.isEmpty() && raster(widget) == beforePreedit, "Cancelled composition clears without sending");
        ime(widget, QString(1025, 'x'));
        require(raster(widget) == beforePreedit && sent.isEmpty(), "Oversized preedit refused locally");
        ime(widget, {}, "wrong replacement", -1);
        require(sent.isEmpty(), "Unsupported remote-text replacement sends no guessed backspaces");
        ime(widget, {}, QString::fromUtf8("日本語"));
        require(sent == QString::fromUtf8("日本語").toUtf8(), "Confirmed composition sent once"); sent.clear();
        ime(widget, {}, QString(65537, 'x'));
        require(sent.isEmpty() && failures == 0, "Oversized commit refused before UTF-8 copy");
        widget.feed("private remote output");
        QInputMethodQueryEvent query(Qt::ImSurroundingText | Qt::ImCurrentSelection | Qt::ImCursorPosition);
        QApplication::sendEvent(&widget, &query);
        require(query.value(Qt::ImSurroundingText).toString().isEmpty() &&
            query.value(Qt::ImCurrentSelection).toString().isEmpty(), "IME never receives remote surrounding text");

        QTextLayout fallback(QString::fromUtf8("界"), primary);
        fallback.beginLayout(); auto line = fallback.createLine(); line.setLineWidth(100); fallback.endLayout();
        bool found = false;
        for (const auto& run : fallback.glyphRuns()) {
            const auto raw = run.rawFont();
            if (raw.supportsCharacter(0x754c)) {
                found = true;
                std::cout << "Verified CJK font fallback: " << raw.familyName().toStdString() << '\n';
            }
        }
        require(found, "Fixture needs a real installed CJK fallback font, not tofu");
        require(widget.isEnabled() && failures == 0, "Presentation changes do not poison terminal");
        std::cout << "Native cursor/preedit/font presentation checks passed\n"; return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
