#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QTest>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace nordssh;
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static void select(TerminalModel& model, QPointF from, QPointF to) {
    model.beginSelection(from); model.extendSelection(to); model.endSelection();
}
static void mouse(TerminalWidget& widget, QEvent::Type type, QPointF position,
    Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent event(type, position, position, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
    QApplication::sendEvent(&widget, &event);
}
static void key(TerminalWidget& widget, QEvent::Type type, Qt::KeyboardModifiers mods,
    bool repeat = false, int code = Qt::Key_C, quint32 native = 0) {
    QKeyEvent event(type, code, mods, 0, native, 0, {}, repeat);
    QApplication::sendEvent(&widget, &event);
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        TerminalModel model(20, 4); model.resize(20, 4, 10, 20);
        require(!model.selectionText(), "No selection cannot overwrite clipboard");
        model.feed("hello world\r\nsecond line");
        select(model, {2, 5}, {48, 5});
        require(model.selectionText() == QString("hello"), "Forward selection includes endpoint cells");
        model.beginSelection({12, 5}, TerminalSelectionKind::Word);
        require(model.selectionText() == QString("hello"), "Word selection on press");
        model.extendSelection({88, 5}); model.endSelection();
        require(model.selectionText() == QString("hello world"), "Word-granular drag");
        model.beginSelection({12, 5}, TerminalSelectionKind::Line); model.endSelection();
        require(model.selectionText() == QString("hello world"), "Line selection on press");
        select(model, {2, 5}, {48, 5});
        auto frame = model.frame();
        require(frame.cells[0].selected && frame.cells[4].selected && !frame.cells[5].selected,
            "Render selection matches copied range");
        select(model, {48, 5}, {2, 5});
        require(model.selectionText() == QString("hello"), "Reverse drag");
        select(model, {2, 5}, {58, 25});
        require(model.selectionText() == QString("hello world\nsecond"), "Hard newline and trailing space trim");
        model.beginSelection({5, 5}); model.endSelection();
        require(!model.selectionText(), "Single click clears old selection without copying");
        model.feed("\x1b[2J\x1b[H");
        model.feed(QString::fromUtf8("A界éZ").toUtf8());
        select(model, {2, 5}, {48, 5});
        require(model.selectionText() == QString::fromUtf8("A界éZ"), "Wide and combining Unicode copy");
        select(model, {12, 5}, {28, 5});
        require(model.selectionText() == QString::fromUtf8("界"), "Wide character copies once, not a continuation cell");
        frame = model.frame();
        require(frame.cells[1].selected && frame.cells[2].selected, "Both display cells highlight a selected wide character");
        select(model, {2, 5}, {48, 5});
        model.feed("\r\nnew output");
        require(model.selectionText() == QString::fromUtf8("A界éZ"), "Completed selection survives unrelated output");
        model.resize(10, 4, 10, 20);
        require(model.selectionText() == QString::fromUtf8("A界éZ"), "Engine tracks selection across resize");
        model.feed("\x1b[?1049h\x1b[2J\x1b[Hsecret alternate");
        require(!model.selectionText(), "Primary selection not copied on alternate screen");
        select(model, {2, 5}, {58, 5});
        require(model.selectionText().has_value(), "Alternate screen has its own selection");
        model.feed("\x1b[?1049l");
        require(!model.selectionText(), "Alternate selection does not leak back");
        bool rejected = false;
        try { model.beginSelection({std::numeric_limits<double>::quiet_NaN(), 1}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Reject nonfinite positions before ABI");

        TerminalModel wrapped(5, 4); wrapped.resize(5, 4, 10, 20); wrapped.feed("abcdefghij");
        select(wrapped, {2, 5}, {48, 25});
        require(wrapped.selectionText() == QString("abcdefghij"), "Soft wraps do not add newlines");
        TerminalModel history(20, 3); history.resize(20, 3, 10, 20);
        history.feed("first\r\nsecond\r\nthird\r\nfourth"); history.scrollToTop();
        select(history, {2, 5}, {48, 5});
        require(history.selectionText() == QString("first"), "Selection uses viewport history coordinates");
        history.feed("\r\nfifth");
        require(history.selectionText() == QString("first"), "Tracked history selection survives output");
        history.beginSelection({2, 5}); history.feed("\r\nsixth"); history.extendSelection({48, 5}); history.endSelection();
        require(history.selectionText().has_value(), "Tracked active gesture survives ordinary output");
        history.beginSelection({2, 5}); history.feed("\x1b[?1049h\x1b[?1049l");
        history.extendSelection({48, 5}); history.endSelection();
        require(history.frame().columns == 20, "Switch and return within a batch cannot dereference a stale gesture");

        TerminalModel large(512, 96); large.resize(512, 96, 10, 20);
        QByteArray cluster("e");
        for (int i = 0; i < 16; ++i) cluster += QByteArray::fromHex("cc81");
        const auto row = cluster.repeated(512); // 16,896 UTF-8 bytes, below MaximumBatch.
        for (int i = 0; i < 64; ++i) large.feed(row);
        select(large, {2, 5}, {5118, 1265});
        require(!large.selectionText(), "Copy above 1 MiB refuses instead of truncating or allocating unbounded output");
        require(large.frame().columns == 512, "Refused copy does not disable terminal");
        select(large, {2, 5}, {508, 5});
        require(large.selectionText().has_value(), "Smaller subsequent copy still works");

        TerminalWidget widget; widget.resize(widget.sizeHint()); widget.show(); app.processEvents();
        QByteArray sent; QString copied("untouched"); int writes = 0, failures = 0;
        widget.setSend([&](const QByteArray& bytes) { sent += bytes; });
        widget.setCopySink([&](const QString& text) { ++writes; copied = text; });
        widget.setFailure([&] { ++failures; });
        const auto mods = Qt::ControlModifier | Qt::ShiftModifier;
        key(widget, QEvent::KeyPress, mods); key(widget, QEvent::KeyRelease, mods);
        require(writes == 0 && copied == "untouched" && sent.isEmpty(), "Empty copy has no side effects");
        widget.feed("hello world");
        const int cell = widget.sizeHint().width() / 80;
        const QPointF start(cell * 0.2, 5), end(cell * 4 + cell * 0.8, 5);
        mouse(widget, QEvent::MouseButtonPress, start);
        mouse(widget, QEvent::MouseMove, end);
        mouse(widget, QEvent::MouseButtonRelease, end);
        require(writes == 0, "Selecting never writes to clipboard");
        key(widget, QEvent::KeyPress, mods);
        key(widget, QEvent::KeyPress, Qt::ControlModifier, true);
        key(widget, QEvent::KeyRelease, Qt::ControlModifier);
        require(writes == 1 && copied == "hello" && sent.isEmpty(), "Explicit copy exactly once, including changed-modifier release");
        key(widget, QEvent::KeyPress, Qt::ControlModifier);
        key(widget, QEvent::KeyRelease, Qt::ControlModifier);
        require(sent == QByteArray(1, '\x03'), "Ctrl+C still interrupts"); sent.clear();
        widget.feed("\x1b[?1002h\x1b[?1006h\x1b[>3u");
        mouse(widget, QEvent::MouseButtonPress, start, Qt::ShiftModifier);
        mouse(widget, QEvent::MouseMove, end);
        mouse(widget, QEvent::MouseButtonRelease, end);
        key(widget, QEvent::KeyPress, mods); key(widget, QEvent::KeyRelease, Qt::NoModifier);
        require(writes == 2 && copied == "hello" && sent.isEmpty(), "Shift selection and copy never leak mouse or Kitty release bytes");
        QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_C, mods);
        shortcut.ignore(); QApplication::sendEvent(&widget, &shortcut);
        require(shortcut.isAccepted(), "Parent shortcuts cannot steal terminal Copy");
#ifdef Q_OS_WIN
        key(widget, QEvent::KeyPress, mods, false, Qt::Key_unknown, 0x43);
        key(widget, QEvent::KeyRelease, Qt::NoModifier, false, Qt::Key_unknown, 0x43);
        require(writes == 3 && sent.isEmpty(), "Physical C copy in Cyrillic layout");
#endif
        mouse(widget, QEvent::MouseButtonPress, start, Qt::ShiftModifier);
        QFocusEvent lost(QEvent::FocusOut); QApplication::sendEvent(&widget, &lost);
        mouse(widget, QEvent::MouseMove, end);
        mouse(widget, QEvent::MouseButtonRelease, end);
        require(sent.isEmpty() && failures == 0, "Focus loss cancels local drag without remote release");
        mouse(widget, QEvent::MouseButtonPress, start, Qt::ShiftModifier);
        QMouseEvent lostButton(QEvent::MouseMove, end, end, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &lostButton);
        mouse(widget, QEvent::MouseButtonRelease, end);
        key(widget, QEvent::KeyPress, mods); key(widget, QEvent::KeyRelease, mods);
        require(copied == "hello" && sent.isEmpty(), "Lost local button state does not select or overwrite clipboard");
        mouse(widget, QEvent::MouseButtonDblClick, start, Qt::ShiftModifier);
        mouse(widget, QEvent::MouseButtonRelease, start);
        key(widget, QEvent::KeyPress, mods); key(widget, QEvent::KeyRelease, mods);
        require(copied == "hello", "Double click selects word locally in mouse mode");
        mouse(widget, QEvent::MouseButtonPress, start, Qt::ShiftModifier);
        mouse(widget, QEvent::MouseButtonRelease, start);
        key(widget, QEvent::KeyPress, mods); key(widget, QEvent::KeyRelease, mods);
        require(copied == "hello world" && sent.isEmpty(), "Third nearby click selects whole line without remote bytes");
        TerminalWidget scrolling; scrolling.resize(scrolling.sizeHint()); scrolling.show(); app.processEvents();
        for (int i = 0; i < 80; ++i) scrolling.feed("scroll-row-" + QByteArray::number(i) + "\r\n");
        const auto rowText = [](TerminalFrame value) {
            QString text; for (int x = 0; x < value.columns; ++x) text += value.cells[x].text;
            return text;
        };
        const auto initial = rowText(scrolling.snapshot());
        mouse(scrolling, QEvent::MouseButtonPress, {5, 35});
        mouse(scrolling, QEvent::MouseMove, {5, -20});
        auto* selectionTimer = scrolling.findChild<QTimer*>("terminalSelectionTimer");
        require(selectionTimer && selectionTimer->isActive(), "Outside local drag starts bounded scroll timer");
        QTest::qWait(120);
        require(rowText(scrolling.snapshot()) != initial, "Drag scroll reveals older history");
        mouse(scrolling, QEvent::MouseButtonRelease, {5, -20});
        require(!selectionTimer->isActive(), "Release stops autoscroll");
        mouse(scrolling, QEvent::MouseButtonPress, {5, 35});
        mouse(scrolling, QEvent::MouseMove, {5, -20});
        scrolling.feed("\x1b[?1049h\x1b[2J\x1b[HALT"); QTest::qWait(100);
        require(!selectionTimer->isActive(), "Screen switch invalidates drag before another scroll");
        mouse(scrolling, QEvent::MouseButtonRelease, {5, -20});
        auto* closing = new TerminalWidget;
        closing->resize(closing->sizeHint()); closing->show(); app.processEvents(); closing->feed("hello");
        mouse(*closing, QEvent::MouseButtonPress, start); mouse(*closing, QEvent::MouseMove, end);
        mouse(*closing, QEvent::MouseButtonRelease, end);
        closing->setCopySink([&](const QString&) { delete closing; closing = nullptr; });
        key(*closing, QEvent::KeyPress, mods);
        require(closing == nullptr, "Copy callback may destroy terminal");
        std::cout << "Native terminal selection/copy checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return EXIT_FAILURE;
    }
}
