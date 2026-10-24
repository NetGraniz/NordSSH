#include "TerminalWidget.h"
#include "input/PasteRouter.h"
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QWheelEvent>
#include <algorithm>
#include <utility>

namespace nordssh {
namespace {
unsigned historyKey(int key) {
    switch (key) {
    case Qt::Key_PageUp: return 1;
    case Qt::Key_PageDown: return 2;
    case Qt::Key_Home: return 4;
    case Qt::Key_End: return 8;
    default: return 0;
    }
}
bool copyKey(const QKeyEvent& event) {
#ifdef Q_OS_WIN
    if (event.nativeVirtualKey() == 0x43) return true;
#endif
    return event.key() == Qt::Key_C;
}
bool copyModifiers(const QKeyEvent& event) {
    return event.modifiers() == (Qt::ControlModifier | Qt::ShiftModifier);
}
bool findKey(const QKeyEvent& event) {
#ifdef Q_OS_WIN
    if (event.nativeVirtualKey() == 0x46) return true;
#endif
    return event.key() == Qt::Key_F;
}
}
TerminalWidget::TerminalWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setAccessibleName(tr("SSH terminal"));
    updateMetrics();
    readPaste_ = [] { return QApplication::clipboard()->text(); };
    writeCopy_ = [](const QString& text) { QApplication::clipboard()->setText(text, QClipboard::Clipboard); };
    confirmPaste_ = [this] {
        auto* dialog = new QMessageBox(QMessageBox::Warning, tr("Paste commands"),
            tr("This text contains line breaks or terminal controls. Paste it?"),
            QMessageBox::Yes | QMessageBox::No, this);
        QPointer<QMessageBox> alive(dialog);
        dialog->setTextFormat(Qt::PlainText);
        dialog->setDefaultButton(QMessageBox::No);
        const int result = dialog->exec();
        if (!alive) return false;
        delete dialog;
        return result == QMessageBox::Yes;
    };
    pasteRouter_ = new PasteRouter(this, [this] { paste(); });
    cursorTimer_ = new QTimer(this);
    cursorTimer_->setObjectName("terminalCursorTimer");
    cursorTimer_->setInterval(std::max(100, QApplication::cursorFlashTime() / 2));
    connect(cursorTimer_, &QTimer::timeout, this, [this] {
        updateCursorBlink();
        if (cursorTimer_->isActive()) { cursorPhase_ = !cursorPhase_; update(); }
    });
    frame_ = model_.frame();
}
void TerminalWidget::setSend(std::function<void(const QByteArray&)> send) { send_ = std::move(send); }
void TerminalWidget::setResize(std::function<void(int, int)> resize) { resize_ = std::move(resize); }
void TerminalWidget::setFailure(std::function<void()> failure) { failure_ = std::move(failure); }
void TerminalWidget::setRightClickPaste(bool enabled) { pasteRouter_->setRightClickPaste(enabled); }
void TerminalWidget::setCopySink(std::function<void(const QString&)> write) { writeCopy_ = std::move(write); }
void TerminalWidget::setPasteSource(std::function<QString()> read, std::function<bool()> confirm) {
    readPaste_ = std::move(read); confirmPaste_ = std::move(confirm);
}
void TerminalWidget::openSearch() {
    if (failed_) return;
    QPointer<TerminalWidget> self(this);
    if (!searchBar_) {
        searchBar_ = new QFrame(this);
        searchBar_->setObjectName("terminalSearchBar");
        searchBar_->setAutoFillBackground(true);
        auto* layout = new QHBoxLayout(searchBar_);
        layout->setContentsMargins(6, 4, 6, 4);
        searchInput_ = new QLineEdit(searchBar_);
        searchInput_->setObjectName("terminalSearchInput");
        searchInput_->setPlaceholderText(tr("Find in terminal"));
        searchInput_->setAccessibleName(tr("Find in terminal"));
        searchInput_->setToolTip(tr("Literal search. ASCII ignores case; other characters match exactly."));
        searchInput_->setMaxLength(TerminalModel::MaximumSearchBytes);
        searchInput_->installEventFilter(this);
        searchCount_ = new QLabel(searchBar_); searchCount_->setTextFormat(Qt::PlainText);
        searchCount_->setObjectName("terminalSearchCount");
        searchOlder_ = new QPushButton(tr("Older"), searchBar_);
        searchNewer_ = new QPushButton(tr("Newer"), searchBar_);
        auto* close = new QPushButton(tr("Close"), searchBar_);
        for (auto* button : {searchOlder_, searchNewer_, close}) button->installEventFilter(this);
        layout->addWidget(searchInput_, 1); layout->addWidget(searchCount_);
        layout->addWidget(searchOlder_); layout->addWidget(searchNewer_); layout->addWidget(close);
        searchTimer_ = new QTimer(this); searchTimer_->setObjectName("terminalSearchTimer"); searchTimer_->setInterval(40);
        connect(searchTimer_, &QTimer::timeout, this, [this] { searchStep(); });
        connect(searchInput_, &QLineEdit::textChanged, this, [this](const QString& text) {
            QPointer<TerminalWidget> self(this);
            try {
                if (!model_.setSearch(text)) {
                    static_cast<void>(model_.setSearch({}));
                    searchCount_->setText(tr("Invalid query or UTF-8 limit exceeded"));
                    searchOlder_->setEnabled(false); searchNewer_->setEnabled(false);
                    searchTimer_->stop(); return;
                }
                searchSelectFirst_ = !text.isEmpty();
                if (isVisible()) searchTimer_->start();
                searchStep();
            } catch (...) { if (self) self->fail(); }
        });
        connect(searchOlder_, &QPushButton::clicked, this, [this] { searchMove(true); });
        connect(searchNewer_, &QPushButton::clicked, this, [this] { searchMove(false); });
        connect(close, &QPushButton::clicked, this, [this] { closeSearch(); });
    }
    searchBar_->show(); searchBar_->raise(); searchInput_->setFocus();
    if (!self) return; // Focus loss can release a remote button and close the tab.
    searchInput_->selectAll();
    searchTimer_->start(); searchStep();
    if (self) refreshGeometry();
}
void TerminalWidget::closeSearch() {
    if (!searchBar_) return;
    QPointer<TerminalWidget> self(this);
    searchTimer_->stop(); searchInput_->clear();
    if (!self) return;
    searchTimer_->stop();
    searchBar_->hide(); setFocus();
    if (self) refreshGeometry();
}
bool TerminalWidget::eventFilter(QObject* target, QEvent* event) {
    if (searchBar_ && searchBar_->isVisible() &&
        (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease || event->type() == QEvent::ShortcutOverride)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            event->accept();
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
                if (key->key() == Qt::Key_Escape) { searchEscapeHeld_ = true; closeSearch(); }
                else searchMove(!key->modifiers().testFlag(Qt::ShiftModifier));
            }
            return true;
        }
    }
    return QWidget::eventFilter(target, event);
}
void TerminalWidget::searchStep() {
    if (failed_ || !searchBar_ || !searchBar_->isVisible()) return;
    QPointer<TerminalWidget> self(this);
    try {
        auto state = model_.advanceSearch();
        if (state.complete && state.matches && searchSelectFirst_) {
            searchSelectFirst_ = false;
            static_cast<void>(model_.selectSearchMatch()); state = model_.advanceSearch();
            frame_ = model_.frame(); update();
        }
        if (state.limited) searchCount_->setText(tr("Too many matches; narrow search"));
        else if (!state.complete) searchCount_->setText(tr("Searching…"));
        else if (state.selected) searchCount_->setText(tr("%1 of %2").arg(*state.selected + 1).arg(state.matches));
        else searchCount_->setText(tr("%1 matches").arg(state.matches));
        searchOlder_->setEnabled(state.complete && state.matches != 0);
        searchNewer_->setEnabled(state.complete && state.matches != 0);
        if (state.limited || searchInput_->text().isEmpty()) searchTimer_->stop();
    } catch (...) { if (self) self->fail(); }
}
void TerminalWidget::searchMove(bool older) {
    if (failed_) return;
    QPointer<TerminalWidget> self(this);
    try {
        if (model_.selectSearchMatch(older)) { frame_ = model_.frame(); update(); }
        searchStep();
    } catch (...) { if (self) self->fail(); }
}
void TerminalWidget::updateMetrics() {
    const QFontMetrics metrics(font());
    cellWidth_ = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
    cellHeight_ = std::max(1, metrics.height()); baseline_ = metrics.ascent();
}
QSize TerminalWidget::sizeHint() const { return {cellWidth_ * 80, cellHeight_ * 24}; }
void TerminalWidget::fail() noexcept {
    if (failed_) return;
    failed_ = true;
    setEnabled(false);
    try { const auto callback = failure_; if (callback) callback(); } catch (...) {}
}
void TerminalWidget::flush() {
    const auto bytes = model_.takeReplies();
    const auto send = send_;
    if (!bytes.isEmpty() && send) send(bytes);
}
void TerminalWidget::feed(const QByteArray& bytes) {
    if (failed_) return;
    QPointer<TerminalWidget> self(this);
    try {
        model_.feed(bytes); frame_ = model_.frame(); update();
        updateCursorBlink();
        flush(); // Last: callback may close the tab.
    } catch (...) { if (self) self->fail(); }
}
TerminalFrame TerminalWidget::snapshot() { return frame_; }
bool TerminalWidget::event(QEvent* event) {
    // Inactive tabs must not wake every 40 ms merely because find was open.
    if (event->type() == QEvent::Hide && searchTimer_) searchTimer_->stop();
    if (event->type() == QEvent::Hide && cursorTimer_) cursorTimer_->stop();
    if (event->type() == QEvent::Show && cursorTimer_) updateCursorBlink();
    if (event->type() == QEvent::Show && searchTimer_ && searchBar_ &&
        !searchBar_->isHidden() && !searchInput_->text().isEmpty()) searchTimer_->start();
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->modifiers() == Qt::ShiftModifier && historyKey(key->key())) ||
            (copyKey(*key) && copyModifiers(*key)) || (findKey(*key) && copyModifiers(*key))) {
            event->accept(); return true;
        }
    }
    // QWidget normally uses Tab/Backtab for focus navigation before dispatching
    // keyPressEvent. A terminal must deliver these keys to the remote program.
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(key); return true;
        }
    }
    return QWidget::event(event);
}
void TerminalWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), frame_.background);
    // Paint every background first so a wide glyph isn't erased by its tail.
    for (int y = 0; y < frame_.rows; ++y) {
        for (int x = 0; x < frame_.columns; ++x) {
            const auto& cell = frame_.cells[y * frame_.columns + x];
            painter.fillRect(x * cellWidth_, y * cellHeight_, cellWidth_, cellHeight_,
                cell.selected ? palette().color(QPalette::Highlight) : cell.background);
        }
    }
    for (int y = 0; y < frame_.rows; ++y) {
        for (int x = 0; x < frame_.columns; ++x) {
            const auto& cell = frame_.cells[y * frame_.columns + x];
            if (cell.width == 0 || cell.invisible || cell.text.isEmpty()) continue;
            auto glyphFont = font();
            glyphFont.setBold(cell.bold); glyphFont.setItalic(cell.italic);
            glyphFont.setUnderline(cell.underline); glyphFont.setStrikeOut(cell.strike);
            painter.setFont(glyphFont);
            painter.setPen(cell.selected ? palette().color(QPalette::HighlightedText) : cell.foreground);
            painter.save();
            const QRect area(x * cellWidth_, y * cellHeight_, cell.width * cellWidth_, cellHeight_);
            painter.setClipRect(area);
            painter.drawText(QPoint(area.x(), area.y() + baseline_), cell.text);
            painter.restore();
        }
    }
    if (frame_.cursorVisible && preedit_.isEmpty() && (cursorPhase_ || !hasFocus())) {
        int x = frame_.cursorX;
        if (frame_.cursorWideTail && x > 0) --x;
        const auto& cell = frame_.cells[frame_.cursorY * frame_.columns + x];
        const int span = cell.width == 2 ? 2 : 1;
        const QRect area(x * cellWidth_, frame_.cursorY * cellHeight_, span * cellWidth_, cellHeight_);
        const auto shape = hasFocus() ? frame_.cursorShape : TerminalCursorShape::Hollow;
        painter.setPen(frame_.cursorColor);
        if (shape == TerminalCursorShape::Hollow) painter.drawRect(area.adjusted(0, 0, -1, -1));
        else if (shape == TerminalCursorShape::Bar) painter.fillRect(area.x(), area.y(), 2, area.height(), frame_.cursorColor);
        else if (shape == TerminalCursorShape::Underline) painter.fillRect(area.x(), area.bottom() - 1, area.width(), 2, frame_.cursorColor);
        else {
            painter.fillRect(area, frame_.cursorColor);
            if (!cell.invisible && !cell.text.isEmpty()) {
                auto glyphFont = font(); glyphFont.setBold(cell.bold); glyphFont.setItalic(cell.italic);
                glyphFont.setUnderline(cell.underline); glyphFont.setStrikeOut(cell.strike);
                painter.setFont(glyphFont); painter.setPen(cell.background);
                painter.save(); painter.setClipRect(area);
                painter.drawText(QPoint(area.x(), area.y() + baseline_), cell.text); painter.restore();
            }
        }
    }
    paintPreedit(painter);
}
void TerminalWidget::updateCursorBlink() {
    if (!cursorTimer_) return;
    const bool blink = !failed_ && isVisible() && hasFocus() && frame_.cursorVisible &&
        frame_.cursorBlink && preedit_.isEmpty() && QApplication::cursorFlashTime() > 0;
    if (!blink) { cursorTimer_->stop(); cursorPhase_ = true; }
    else if (!cursorTimer_->isActive()) { cursorPhase_ = true; cursorTimer_->start(); }
}
void TerminalWidget::paintPreedit(QPainter& painter) {
    if (preedit_.isEmpty() || !frame_.cursorVisible || !hasFocus()) return;
    QTextLayout layout(preedit_, font());
    QTextOption option; option.setWrapMode(QTextOption::NoWrap); layout.setTextOption(option);
    layout.setFormats(preeditFormats_);
    layout.beginLayout(); auto line = layout.createLine();
    line.setLineWidth(frame_.columns * cellWidth_); layout.endLayout();
    const qreal y = frame_.cursorY * cellHeight_;
    const qreal x = std::min<qreal>(frame_.cursorX * cellWidth_,
        std::max<qreal>(0, frame_.columns * cellWidth_ - line.naturalTextWidth()));
    const QRectF clip(0, y, frame_.columns * cellWidth_, cellHeight_);
    painter.save(); painter.setClipRect(clip);
    painter.fillRect(QRectF(x, y, std::min<qreal>(line.naturalTextWidth() + 2, width()), cellHeight_), frame_.background);
    painter.setPen(palette().color(QPalette::Text)); layout.draw(&painter, {x, y});
    if (preeditCursorVisible_) layout.drawCursor(&painter, {x, y}, preeditCursor_, 2);
    painter.restore();
}
void TerminalWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refreshGeometry();
}
void TerminalWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateMetrics(); updateGeometry(); refreshGeometry();
    }
}
void TerminalWidget::refreshGeometry() {
    if (failed_) return;
    QPointer<TerminalWidget> self(this);
    try {
        updateMetrics();
        const int columns = std::clamp(width() / cellWidth_, 1, 512);
        const int barHeight = searchBar_ && searchBar_->isVisible() ? searchBar_->sizeHint().height() : 0;
        if (searchBar_) searchBar_->setGeometry(0, std::max(0, height() - barHeight), width(), barHeight);
        const int rows = std::clamp((height() - barHeight) / cellHeight_, 1, 256);
        model_.resize(columns, rows, cellWidth_, cellHeight_);
        localSelection_ = false;
        frame_ = model_.frame(); update();
        const auto callback = resize_;
        if (callback) callback(columns, rows);
    } catch (...) { if (self) self->fail(); }
}
void TerminalWidget::keyPressEvent(QKeyEvent* event) {
    event->accept();
    if (failed_) return;
    QPointer<TerminalWidget> self(this);
    try {
        if (searchShortcut(event)) return;
        if (copyShortcut(event)) return;
        if (historyNavigation(event)) return;
        const auto bytes = model_.key(*event);
        const auto send = send_;
        if (!bytes.isEmpty() && send) { followOutput(); send(bytes); }
    } catch (...) { if (self) self->fail(); }
}
void TerminalWidget::keyReleaseEvent(QKeyEvent* event) { keyPressEvent(event); }
bool TerminalWidget::searchShortcut(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && searchEscapeHeld_) {
        if (event->type() == QEvent::KeyRelease) {
            if (!event->isAutoRepeat()) searchEscapeHeld_ = false;
            return true;
        }
        if (event->isAutoRepeat()) return true;
        searchEscapeHeld_ = false;
    }
    if (!findKey(*event)) return false;
    if (event->type() == QEvent::KeyRelease) {
        if (!searchKeyHeld_) return false;
        if (!event->isAutoRepeat()) searchKeyHeld_ = false;
        return true;
    }
    if (event->isAutoRepeat() && searchKeyHeld_) return true;
    searchKeyHeld_ = false;
    if (!copyModifiers(*event)) return false;
    searchKeyHeld_ = true; openSearch(); return true;
}
bool TerminalWidget::copyShortcut(QKeyEvent* event) {
    if (!copyKey(*event)) return false;
    if (event->type() == QEvent::KeyRelease) {
        if (!copyKeyHeld_) return false;
        if (!event->isAutoRepeat()) copyKeyHeld_ = false;
        return true;
    }
    // Once claimed, repeats and the eventual release stay local even if the
    // user releases Shift first. Ctrl+C on a new gesture remains an interrupt.
    if (event->isAutoRepeat() && copyKeyHeld_) return true;
    copyKeyHeld_ = false;
    if (!copyModifiers(*event)) return false;
    copyKeyHeld_ = true;
    const auto text = model_.selectionText();
    const auto write = writeCopy_;
    if (text && write && !event->isAutoRepeat()) write(*text); // May close this tab.
    return true;
}
bool TerminalWidget::historyNavigation(QKeyEvent* event) {
    const auto bit = historyKey(event->key());
    if (!bit) return false;
    if (event->type() == QEvent::KeyRelease) {
        if (!(historyKeys_ & bit)) return false;
        if (!event->isAutoRepeat()) historyKeys_ &= ~bit;
        return true;
    }
    if (event->modifiers() != Qt::ShiftModifier) {
        historyKeys_ &= ~bit; return false;
    }
    historyKeys_ |= bit;
    if (event->key() == Qt::Key_Home) model_.scrollToTop();
    else if (event->key() == Qt::Key_End) model_.scrollToBottom();
    else model_.scroll((event->key() == Qt::Key_PageUp ? -1 : 1) * std::max(1, frame_.rows - 1));
    frame_ = model_.frame(); update(); return true;
}
void TerminalWidget::followOutput() {
    if (frame_.followingOutput) return;
    model_.scrollToBottom(); frame_ = model_.frame(); update();
}
void TerminalWidget::wheelEvent(QWheelEvent* event) {
    event->accept();
    if (failed_) return;
    QPointer<TerminalWidget> self(this);
    try {
        const bool remote = frame_.mouseTracking && !event->modifiers().testFlag(Qt::ShiftModifier);
        if (remote != wheelWasRemote_) { remoteWheel_ = wheelAngle_ = wheelPixels_ = 0; wheelWasRemote_ = remote; }
        if (remote) {
            const bool pixels = !event->pixelDelta().isNull() && QGuiApplication::platformName() != "xcb";
            if (pixels != remoteWheelPixels_) { remoteWheel_ = 0; remoteWheelPixels_ = pixels; }
            const int unit = pixels ? cellHeight_ : 120;
            remoteWheel_ = std::clamp<qint64>(remoteWheel_ + (pixels ? event->pixelDelta().y() : event->angleDelta().y()), -32768, 32768);
            const int steps = static_cast<int>(std::clamp<qint64>(remoteWheel_ / unit, -32, 32));
            remoteWheel_ %= unit; // Excess events are bounded, not queued without limit.
            const auto bytes = model_.wheel(*event, steps);
            const auto send = send_;
            if (!bytes.isEmpty() && send) { followOutput(); send(bytes); }
            return;
        }
        qint64 lines = 0;
        if (!event->pixelDelta().isNull() && QGuiApplication::platformName() != "xcb") {
            wheelAngle_ = 0;
            wheelPixels_ = std::clamp<qint64>(wheelPixels_ + event->pixelDelta().y(), -32768, 32768);
            lines = -wheelPixels_ / cellHeight_; wheelPixels_ %= cellHeight_;
        } else {
            wheelPixels_ = 0;
            wheelAngle_ = std::clamp<qint64>(wheelAngle_ + event->angleDelta().y(), -12000, 12000);
            lines = -(wheelAngle_ / 120) * 3; wheelAngle_ %= 120;
        }
        // Qt deltas already encode the platform's natural-scrolling direction;
        // inverted() is informative, not a reason to invert them a second time.
        if (lines == 0) return;
        model_.scroll(static_cast<int>(std::clamp<qint64>(lines,-10000,10000))); frame_ = model_.frame(); update();
    } catch (...) { if (self) self->fail(); }
}
void TerminalWidget::mouseInput(QMouseEvent* event) {
    event->accept();
    if (failed_) return;
    QPointer<TerminalWidget> self(this);
    try {
        lastMouse_ = event->position();
        const bool release = event->type() == QEvent::MouseButtonRelease;
        const bool motion = event->type() == QEvent::MouseMove;
        if (!motion && event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton && event->button() != Qt::RightButton) return;
        if (localSelection_ && (motion || (release && event->button() == Qt::LeftButton))) {
            if (motion && !event->buttons().testFlag(Qt::LeftButton)) {
                model_.endSelection(); localSelection_ = false; return;
            }
            model_.extendSelection(event->position());
            if (release) { model_.endSelection(); localSelection_ = false; }
            frame_ = model_.frame(); update(); return;
        }
        if (!motion && !release && event->button() == Qt::LeftButton &&
            (!frame_.mouseTracking || event->modifiers().testFlag(Qt::ShiftModifier))) {
            if (event->position().x() < 0 || event->position().y() < 0 ||
                event->position().x() >= frame_.columns * cellWidth_ ||
                event->position().y() >= frame_.rows * cellHeight_) return;
            model_.beginSelection(event->position()); localSelection_ = true;
            frame_ = model_.frame(); update(); return;
        }
        if (release) {
            if (!remoteButtons_.testFlag(event->button())) return;
            remoteButtons_ &= ~event->button();
        } else if (!motion) {
            if (!frame_.mouseTracking || event->modifiers().testFlag(Qt::ShiftModifier)) return;
            if (event->position().x() < 0 || event->position().y() < 0 ||
                event->position().x() >= frame_.columns * cellWidth_ ||
                event->position().y() >= frame_.rows * cellHeight_) return;
            remoteButtons_ |= event->button();
        } else if (!frame_.mouseTracking ||
            (remoteButtons_ == Qt::NoButton && (event->buttons() != Qt::NoButton || event->modifiers().testFlag(Qt::ShiftModifier)))) return;
        // Local Shift gestures and paste-button drags never acquire remote buttons.
        QMouseEvent filtered(event->type(), event->position(), event->globalPosition(),
            event->button(), remoteButtons_, event->modifiers());
        const auto bytes = model_.mouse(filtered);
        const auto send = send_;
        if (!bytes.isEmpty() && send) { followOutput(); send(bytes); }
    } catch (...) { if (self) self->fail(); }
}
void TerminalWidget::mousePressEvent(QMouseEvent* event) { mouseInput(event); }
void TerminalWidget::mouseDoubleClickEvent(QMouseEvent* event) { mouseInput(event); }
void TerminalWidget::mouseReleaseEvent(QMouseEvent* event) { mouseInput(event); }
void TerminalWidget::mouseMoveEvent(QMouseEvent* event) { mouseInput(event); }
void TerminalWidget::focusOutEvent(QFocusEvent* event) {
    preedit_.clear(); preeditFormats_.clear(); updateCursorBlink(); update();
    model_.endSelection(); localSelection_ = false;
    // Keep local-release suppression if focus changes while a navigation key
    // is held. A subsequent nonlocal press explicitly replaces that state.
    wheelAngle_ = wheelPixels_ = remoteWheel_ = 0;
    // Release tracked remote buttons on focus loss; never invent a press.
    QPointer<TerminalWidget> self(this);
    for (const auto button : {Qt::LeftButton, Qt::MiddleButton, Qt::RightButton}) {
        if (!remoteButtons_.testFlag(button)) continue;
        QMouseEvent release(QEvent::MouseButtonRelease, lastMouse_, lastMouse_, button,
            remoteButtons_ & ~button, Qt::NoModifier);
        mouseInput(&release);
        if (!self) return;
    }
    QWidget::focusOutEvent(event);
}
void TerminalWidget::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event); updateCursorBlink(); update();
}
void TerminalWidget::paste() {
    if (failed_ || !send_ || !readPaste_) return;
    QPointer<TerminalWidget> self(this);
    try {
        const auto read = readPaste_;
        const auto captured = read();
        if (!self) return;
        if (captured.size() > TerminalModel::MaximumPaste) return;
        const auto text = captured.toUtf8();
        if (text.size() > TerminalModel::MaximumPaste) return;
        if (!model_.paste(text)) {
            const auto confirm = confirmPaste_;
            const bool accepted = confirm && confirm();
            if (!self || !accepted) return;
            // The tab can survive a nested dialog while its session closes.
            if (failed_ || !send_) return;
            if (!model_.paste(text, true)) return;
        }
        followOutput(); flush();
    } catch (...) { if (self) self->fail(); }
}
void TerminalWidget::inputMethodEvent(QInputMethodEvent* event) {
    event->accept();
    if (failed_) return;
    QPointer<TerminalWidget> self(this);
    try {
        preedit_.clear(); preeditFormats_.clear(); preeditCursor_ = 0; preeditCursorVisible_ = true;
        const auto& composing = event->preeditString();
        if (composing.size() <= 1024 && composing.isValidUtf16() &&
            std::ranges::none_of(composing, [](QChar ch) { return ch.unicode() < 0x20 || ch.unicode() == 0x7f; })) {
            preedit_ = composing; preeditCursor_ = static_cast<int>(composing.size());
            QTextCharFormat base; base.setUnderlineStyle(QTextCharFormat::SingleUnderline);
            preeditFormats_.append({0, static_cast<int>(composing.size()), base});
            int count = 0;
            for (const auto& attribute : event->attributes()) {
                if (++count > 64) break;
                if (attribute.type == QInputMethodEvent::Cursor) {
                    preeditCursor_ = std::clamp(attribute.start, 0, static_cast<int>(composing.size()));
                    preeditCursorVisible_ = attribute.length != 0;
                } else if (attribute.type == QInputMethodEvent::TextFormat && attribute.value.canConvert<QTextFormat>()) {
                    const int start = std::clamp(attribute.start, 0, static_cast<int>(composing.size()));
                    const int length = std::clamp(attribute.length, 0, static_cast<int>(composing.size()) - start);
                    const auto incoming = attribute.value.value<QTextFormat>().toCharFormat();
                    QTextCharFormat format;
                    if (incoming.foreground().style() == Qt::SolidPattern) format.setForeground(incoming.foreground());
                    if (incoming.background().style() == Qt::SolidPattern) format.setBackground(incoming.background());
                    format.setUnderlineStyle(incoming.underlineStyle());
                    preeditFormats_.append({start, length, format});
                }
            }
        }
        updateCursorBlink(); update();
        // A remote terminal has no locally editable surrounding document.
        // Never translate IME replacement requests into guessed Backspaces.
        if (event->replacementStart() != 0 || event->replacementLength() != 0 ||
            event->commitString().isEmpty() || event->commitString().size() > TerminalModel::MaximumPaste ||
            !event->commitString().isValidUtf16()) return;
        // IME commits are text, not a second key/paste-shortcut event. Reuse
        // terminal text sanitization without adding an Enter of our own.
        const auto text = event->commitString().toUtf8();
        if (text.size() > TerminalModel::MaximumPaste || !model_.paste(text)) return;
        followOutput(); flush();
    } catch (...) { if (self) self->fail(); }
}
QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImSurroundingText || query == Qt::ImCurrentSelection) return QString{};
    if (query == Qt::ImCursorPosition || query == Qt::ImAnchorPosition) return 0;
    if (query == Qt::ImHints) return QVariant::fromValue(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase |
        (frame_.passwordInput ? Qt::ImhSensitiveData | Qt::ImhHiddenText : Qt::ImhNone));
    if (query == Qt::ImCursorRectangle)
        return QRect(frame_.cursorX * cellWidth_, frame_.cursorY * cellHeight_, cellWidth_, cellHeight_);
    return QWidget::inputMethodQuery(query);
}
} // namespace nordssh
