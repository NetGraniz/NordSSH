#include "TerminalWidget.h"
#include "input/PasteRouter.h"
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMessageBox>
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
}
TerminalWidget::TerminalWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFocusPolicy(Qt::StrongFocus);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setAccessibleName(tr("SSH terminal"));
    updateMetrics();
    readPaste_ = [] { return QApplication::clipboard()->text(); };
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
    new PasteRouter(this, [this] { paste(); });
    frame_ = model_.frame();
}
void TerminalWidget::setSend(std::function<void(const QByteArray&)> send) { send_ = std::move(send); }
void TerminalWidget::setResize(std::function<void(int, int)> resize) { resize_ = std::move(resize); }
void TerminalWidget::setFailure(std::function<void()> failure) { failure_ = std::move(failure); }
void TerminalWidget::setPasteSource(std::function<QString()> read, std::function<bool()> confirm) {
    readPaste_ = std::move(read); confirmPaste_ = std::move(confirm);
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
        flush(); // Last: callback may close the tab.
    } catch (...) { if (self) self->fail(); }
}
TerminalFrame TerminalWidget::snapshot() { return frame_; }
bool TerminalWidget::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers() == Qt::ShiftModifier && historyKey(key->key())) {
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
            painter.fillRect(x * cellWidth_, y * cellHeight_, cellWidth_, cellHeight_, cell.background);
        }
    }
    for (int y = 0; y < frame_.rows; ++y) {
        for (int x = 0; x < frame_.columns; ++x) {
            const auto& cell = frame_.cells[y * frame_.columns + x];
            if (cell.width == 0 || cell.invisible || cell.text.isEmpty()) continue;
            auto glyphFont = font();
            glyphFont.setBold(cell.bold); glyphFont.setItalic(cell.italic);
            glyphFont.setUnderline(cell.underline); glyphFont.setStrikeOut(cell.strike);
            painter.setFont(glyphFont); painter.setPen(cell.foreground);
            painter.save();
            const QRect area(x * cellWidth_, y * cellHeight_, cell.width * cellWidth_, cellHeight_);
            painter.setClipRect(area);
            painter.drawText(QPoint(area.x(), area.y() + baseline_), cell.text);
            painter.restore();
        }
    }
    if (frame_.cursorVisible) {
        painter.setPen(QColor(115, 210, 150));
        painter.drawRect(frame_.cursorX * cellWidth_, frame_.cursorY * cellHeight_, cellWidth_ - 1, cellHeight_ - 1);
    }
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
        const int rows = std::clamp(height() / cellHeight_, 1, 256);
        model_.resize(columns, rows, cellWidth_, cellHeight_);
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
        if (historyNavigation(event)) return;
        const auto bytes = model_.key(*event);
        const auto send = send_;
        if (!bytes.isEmpty() && send) { followOutput(); send(bytes); }
    } catch (...) { if (self) self->fail(); }
}
void TerminalWidget::keyReleaseEvent(QKeyEvent* event) { keyPressEvent(event); }
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
    // Remote mouse reporting is a separate pending gate. Do not reinterpret
    // a fullscreen program's mouse events as local history unless Shift is held.
    if (frame_.mouseTracking && event->modifiers() != Qt::ShiftModifier) {
        wheelAngle_ = wheelPixels_ = 0; return;
    }
    try {
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
    } catch (...) { fail(); }
}
void TerminalWidget::focusOutEvent(QFocusEvent* event) {
    // Keep local-release suppression if focus changes while a navigation key
    // is held. A subsequent nonlocal press explicitly replaces that state.
    wheelAngle_ = wheelPixels_ = 0;
    QWidget::focusOutEvent(event);
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
    if (failed_ || event->commitString().isEmpty()) return;
    QPointer<TerminalWidget> self(this);
    try {
        // IME commits are text, not a second key/paste-shortcut event. Reuse
        // terminal text sanitization without adding an Enter of our own.
        const auto text = event->commitString().toUtf8();
        if (text.size() > TerminalModel::MaximumPaste || !model_.paste(text)) return;
        followOutput(); flush();
    } catch (...) { if (self) self->fail(); }
}
QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImCursorRectangle)
        return QRect(frame_.cursorX * cellWidth_, frame_.cursorY * cellHeight_, cellWidth_, cellHeight_);
    return QWidget::inputMethodQuery(query);
}
} // namespace nordssh
