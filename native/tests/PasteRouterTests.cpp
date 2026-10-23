#include "input/PasteRouter.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QShortcut>
#include <QTest>
#include <QWidget>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Receiver final : public QWidget {
public:
    int keys = 0;
    int ime = 0;
protected:
    void keyPressEvent(QKeyEvent* key) override { ++keys; key->accept(); }
    void inputMethodEvent(QInputMethodEvent* event) override { ++ime; event->accept(); }
};
static void keyCycle(Receiver& widget, int key, Qt::KeyboardModifiers modifiers,
                     quint32 native = 0, const QString& text = {}) {
    for (auto type : {QEvent::ShortcutOverride, QEvent::KeyPress, QEvent::KeyRelease}) {
        QKeyEvent event(type, key, modifiers, 0, native, 0, text);
        QApplication::sendEvent(&widget, &event);
    }
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        Receiver widget;
        int pastes = 0;
        nordssh::PasteRouter router(&widget, [&] { ++pastes; });
        keyCycle(widget, Qt::Key_V, Qt::ControlModifier);
        require(pastes == 1 && widget.keys == 0, "Ctrl+V must paste once and consume key");
        keyCycle(widget, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        require(pastes == 2, "Ctrl+Shift+V");
        keyCycle(widget, Qt::Key_Insert, Qt::ShiftModifier);
        require(pastes == 3, "Shift+Insert");
#ifdef Q_OS_WIN
        keyCycle(widget, 0x041c, Qt::ControlModifier, 0x56, QString::fromUtf8("\xd0\xbc"));
        require(pastes == 4, "RU physical V must paste exactly once");
#endif
        const auto baseline = pastes;
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier, {}, true);
        QApplication::sendEvent(&widget, &repeat);
        require(pastes == baseline, "Auto-repeat duplicated paste");
        keyCycle(widget, Qt::Key_C, Qt::ControlModifier);
        require(pastes == baseline && widget.keys == 1, "Ctrl+C must reach terminal");
        keyCycle(widget, Qt::Key_V, Qt::ControlModifier | Qt::AltModifier);
        require(pastes == baseline && widget.keys == 2, "AltGr must not paste");
        QInputMethodEvent ime;
        ime.setCommitString(QString::fromUtf8("\xd1\x82\xd0\xb5\xd1\x81\xd1\x82"));
        QApplication::sendEvent(&widget, &ime);
        require(pastes == baseline && widget.ime == 1, "IME must not be a second paste route");
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent mouse(type, QPointF(10,10), QPointF(10,10), Qt::RightButton,
                              type == QEvent::MouseButtonPress ? Qt::RightButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&widget, &mouse);
        }
        QContextMenuEvent menu(QContextMenuEvent::Mouse, QPoint(10,10));
        QApplication::sendEvent(&widget, &menu);
        require(pastes == baseline + 1, "Right-click and context menu duplicated paste");
        QKeyEvent interruptOverride(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier);
        interruptOverride.ignore();
        QApplication::sendEvent(&widget, &interruptOverride);
        require(interruptOverride.isAccepted(), "Ctrl+C not reserved against parent Copy action");
        Receiver other;
        int reentrant = 0;
        nordssh::PasteRouter nested(&other, [&] {
            ++reentrant;
            keyCycle(other, Qt::Key_V, Qt::ControlModifier);
        });
        keyCycle(other, Qt::Key_V, Qt::ControlModifier);
        require(reentrant == 1, "Nested event loop reentered paste");
        keyCycle(other, Qt::Key_V, Qt::ControlModifier);
        require(reentrant == 2, "Paste route remained busy after completion");
        // Exercise Qt's shortcut dispatch, not only manual KeyPress delivery.
        int copyShortcut = 0;
        QShortcut copy(QKeySequence(Qt::CTRL | Qt::Key_C), &widget);
        QObject::connect(&copy, &QShortcut::activated, [&] { ++copyShortcut; });
        widget.show();
        widget.activateWindow();
        widget.setFocus();
        QApplication::processEvents();
        const auto keysBefore = widget.keys;
        QTest::keyClick(&widget, Qt::Key_C, Qt::ControlModifier);
        require(copyShortcut == 0 && widget.keys > keysBefore, "Parent Copy shortcut stole Ctrl+C");
        std::cout << "Synthetic Qt input routing passed; no OS clipboard or real keyboard layout tested\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
