#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTest>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace nordssh;
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static TerminalSearchState finish(TerminalModel& model) {
    for (int i = 0; i < 10000; ++i) {
        auto state = model.advanceSearch();
        if (state.complete) return state;
    }
    throw std::runtime_error("Incremental search did not converge");
}
static void key(QWidget& widget, QEvent::Type type, int value, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent event(type, value, mods); QApplication::sendEvent(&widget, &event);
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        TerminalModel model(16, 4);
        model.feed("first Error\r\nsecond error\r\nthird ERROR\r\nlast\r\n");
        require(model.setSearch("error"), "Literal query accepted");
        require(!model.selectSearchMatch(), "Dirty search cannot invoke blocking navigation");
        auto state = finish(model);
        require(state.matches == 3 && !state.limited, "ASCII ignores case across history");
        require(model.selectSearchMatch(), "Select newest match");
        require(model.selectionText() == QString("ERROR"), "Match becomes engine selection");
        require(finish(model).selected == 0, "Newest match index");
        require(model.selectSearchMatch(), "Select older match");
        require(model.selectionText() == QString("error") && finish(model).selected == 1, "Navigate older");
        require(model.selectSearchMatch(false) && finish(model).selected == 0, "Navigate newer");
        require(model.selectSearchMatch(false) && finish(model).selected == 2, "Navigation wraps");
        model.feed("fresh error\r\n");
        require(!model.selectSearchMatch(), "Output invalidates navigation until next bounded advance");
        require(finish(model).matches == 4, "Live output updates results");
        model.resize(9, 4, 10, 20);
        require(finish(model).matches == 4, "Results survive reflow");
        model.feed("\x1b[?1049h\x1b[2J\x1b[Herror");
        require(finish(model).matches == 1, "Alternate results stay separate");
        model.feed("\x1b[?1049l");
        require(finish(model).matches == 4, "Primary results restored");
        require(!model.setSearch(QString(257, 'a')), "Query byte cap checked before engine allocation");
        require(!model.setSearch(QString(129, QChar(0x0410))), "UTF-8 cap is not UTF-16 length");
        require(!model.setSearch(QString(QChar(0xd800))), "Unpaired surrogate rejected");
        require(!model.setSearch("a\nb"), "Control query rejected");
        require(finish(model).matches == 4, "Refused query leaves existing search intact");
        require(model.setSearch({}) && finish(model).matches == 0, "Clear releases search state");

        TerminalModel unicode(5, 4);
        unicode.feed(QString::fromUtf8("ab界éZ").toUtf8());
        require(unicode.setSearch(QString::fromUtf8("界éZ")) && finish(unicode).matches == 1,
            "Wide and combining match crosses soft wrap");
        require(unicode.selectSearchMatch() && unicode.selectionText() == QString::fromUtf8("界éZ"), "Unicode matched text");
        unicode.feed("\r\nПривет");
        require(unicode.setSearch(QString::fromUtf8("Привет")) && finish(unicode).matches == 1, "Cyrillic literal search");
        require(unicode.setSearch(QString::fromUtf8("привет")) && finish(unicode).matches == 0, "Do not promise Unicode case folding");

        TerminalModel dense(512, 96); dense.feed(QByteArray(60000, 'x'));
        require(dense.setSearch("x") && finish(dense).limited, "Dense matches stop at retention policy");
        require(dense.setSearch("absent") && finish(dense).matches == 0 && !finish(dense).limited,
            "Narrower query recovers after limit");
        TerminalModel longHistory(80, 24);
        for (int i = 0; i < 5000; ++i) longHistory.feed("row " + QByteArray::number(i) + "\r\n");
        require(longHistory.setSearch("row"), "History query");
        QElapsedTimer clock; clock.start(); state = finish(longHistory);
        require(state.matches > 0 && state.complete, "Incremental history search converges");
        std::cout << "5000-line search, observed elapsed ms: " << clock.elapsed() << '\n';

        TerminalWidget widget; widget.resize(widget.sizeHint()); widget.show(); app.processEvents();
        QByteArray sent; int failures = 0;
        widget.setSend([&](const QByteArray& bytes) { sent += bytes; });
        widget.setFailure([&] { ++failures; }); widget.feed("first needle\r\nsecond needle\x1b[>3u");
        const int originalRows = widget.snapshot().rows;
        const auto mods = Qt::ControlModifier | Qt::ShiftModifier;
        key(widget, QEvent::KeyPress, Qt::Key_F, mods);
        key(widget, QEvent::KeyRelease, Qt::Key_F, Qt::NoModifier);
        auto* input = widget.findChild<QLineEdit*>("terminalSearchInput");
        auto* count = widget.findChild<QLabel*>("terminalSearchCount");
        require(input && input->isVisible() && count && widget.snapshot().rows < originalRows,
            "Native search bar reserves terminal space");
        input->setText("needle");
        clock.restart();
        while (!count->text().contains("of 2") && clock.elapsed() < 2000) QTest::qWait(10);
        require(count->text() == "1 of 2" && sent.isEmpty(), "Find shortcut/query never reach SSH");
        auto* timer = widget.findChild<QTimer*>("terminalSearchTimer");
        require(timer && timer->isActive(), "Live nonempty query timer active");
        widget.hide(); require(!timer->isActive(), "Hidden terminal stops search timer");
        widget.show(); app.processEvents(); require(timer->isActive(), "Showing terminal resumes live query");
        key(*input, QEvent::KeyPress, Qt::Key_Return); key(*input, QEvent::KeyRelease, Qt::Key_Return);
        require(count->text() == "2 of 2", "Enter advances older match");
        key(*input, QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
        key(*input, QEvent::KeyRelease, Qt::Key_Return, Qt::ShiftModifier);
        require(count->text() == "1 of 2", "Shift+Enter advances newer");
        key(*input, QEvent::KeyPress, Qt::Key_Escape);
        key(widget, QEvent::KeyRelease, Qt::Key_Escape);
        require(!input->isVisible() && sent.isEmpty() && failures == 0 && widget.snapshot().rows == originalRows,
            "Escape closes search and its release cannot leak to Kitty terminal");
        widget.openSearch(); input->setText(QString(129, QChar(0x0410)));
        require(count->text().contains("limit"), "UI reports byte limit");
        widget.closeSearch();
        auto* closing = new TerminalWidget;
        closing->resize(closing->sizeHint()); closing->show(); app.processEvents();
        QPointer<TerminalWidget> alive(closing);
        closing->setResize([closing](int, int) { delete closing; });
        closing->openSearch();
        require(!alive, "Opening search may close the tab from the resize callback");
        std::cout << "Native incremental terminal search checks passed\n"; return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
