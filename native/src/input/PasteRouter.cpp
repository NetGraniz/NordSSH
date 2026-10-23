#include "PasteRouter.h"
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QScopeGuard>
#include <stdexcept>
#include <utility>

namespace nordssh {
PasteRouter::PasteRouter(QObject* terminalWidget, std::function<void()> requestPaste)
    : QObject(terminalWidget), requestPaste_(std::move(requestPaste)) {
    if (!terminalWidget || !requestPaste_) throw std::invalid_argument("Paste route needs a widget and action");
    terminalWidget->installEventFilter(this);
}

bool PasteRouter::isPaste(const QKeyEvent& key) {
    auto modifiers = key.modifiers();
    modifiers &= ~Qt::KeypadModifier;
    if (key.key() == Qt::Key_Insert && modifiers == Qt::ShiftModifier) return true;
    if (modifiers != Qt::ControlModifier && modifiers != (Qt::ControlModifier | Qt::ShiftModifier))
        return false;
    if (key.key() == Qt::Key_V) return true;
#ifdef Q_OS_WIN
    // VK_V is layout independent; Qt key()/text() may represent Cyrillic М.
    // Do not use this Windows virtual-key value as an X11/Wayland keycode.
    return key.nativeVirtualKey() == 0x56;
#else
    return key.matches(QKeySequence::Paste);
#endif
}

bool PasteRouter::eventFilter(QObject* target, QEvent* event) {
    if (target != parent()) return false;
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease) {
        const auto& key = *static_cast<QKeyEvent*>(event);
        if (event->type() == QEvent::ShortcutOverride && key.modifiers() == Qt::ControlModifier &&
            (key.key() == Qt::Key_C
#ifdef Q_OS_WIN
             || key.nativeVirtualKey() == 0x43
#endif
            )) {
            // Reserve terminal interrupt against any parent Copy QAction. The
            // actual KeyPress still reaches the terminal; copying uses Ctrl+Shift+C.
            event->accept();
            return true;
        }
        if (!isPaste(key)) return false;
        event->accept();
        if (event->type() == QEvent::KeyPress && !key.isAutoRepeat()) requestOnce();
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseButtonDblClick) {
        const auto& mouse = *static_cast<QMouseEvent*>(event);
        if (mouse.button() != Qt::RightButton) return false;
        event->accept();
        if (event->type() == QEvent::MouseButtonPress) requestOnce();
        return true;
    }
    if (event->type() == QEvent::ContextMenu) {
        event->accept(); // A right click must not also invoke a context-menu paste.
        return true;
    }
    return false; // IME commits and Ctrl+C belong to terminal input, not paste.
}

void PasteRouter::requestOnce() {
    // A confirmation can run a nested event loop or close the tab/receiver. Keep
    // the guard and callable alive independently of this QObject's lifetime.
    const auto busy = busy_;
    if (*busy) return;
    *busy = true;
    const auto reset = qScopeGuard([busy] { *busy = false; });
    const auto action = requestPaste_;
    action();
}
} // namespace nordssh
