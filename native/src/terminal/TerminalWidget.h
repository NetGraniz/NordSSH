#pragma once
#include "TerminalModel.h"
#include <QWidget>
#include <functional>
#include <QTextLayout>
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace nordssh {
class PasteRouter;
// Raster-only QWidget, not QTextEdit or a formatted-text terminal substitute.
// Session controller owns delivery/backpressure and supplies bounded batches.
class TerminalWidget final : public QWidget {
public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    void feed(const QByteArray& bytes);
    void setSend(std::function<void(const QByteArray&)> send);
    void setResize(std::function<void(int, int)> resize);
    void setFailure(std::function<void()> failure);
    void setPasteSource(std::function<QString()> read, std::function<bool()> confirm);
    void setRightClickPaste(bool enabled);
    void setCopySink(std::function<void(const QString&)> write);
    void openSearch();
    void closeSearch();
    [[nodiscard]] TerminalFrame snapshot();
    QSize sizeHint() const override;
protected:
    bool event(QEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void changeEvent(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
private:
    void paste();
    void flush();
    void fail() noexcept;
    void updateMetrics();
    void refreshGeometry();
    bool historyNavigation(QKeyEvent*);
    bool copyShortcut(QKeyEvent*);
    bool searchShortcut(QKeyEvent*);
    void searchStep();
    void searchMove(bool older);
    void followOutput();
    void mouseInput(QMouseEvent*);
    void updateCursorBlink();
    void paintPreedit(QPainter& painter);
    TerminalModel model_;
    TerminalFrame frame_;
    int cellWidth_ = 9, cellHeight_ = 18, baseline_ = 14;
    bool failed_ = false;
    bool localSelection_ = false, copyKeyHeld_ = false;
    bool searchKeyHeld_ = false, searchSelectFirst_ = false, searchEscapeHeld_ = false;
    unsigned historyKeys_ = 0;
    PasteRouter* pasteRouter_ = nullptr;
    Qt::MouseButtons remoteButtons_;
    QPointF lastMouse_;
    qint64 remoteWheel_ = 0;
    bool remoteWheelPixels_ = false, wheelWasRemote_ = false;
    qint64 wheelAngle_ = 0, wheelPixels_ = 0;
    std::function<void(const QByteArray&)> send_;
    std::function<void(int, int)> resize_;
    std::function<void()> failure_;
    std::function<QString()> readPaste_;
    std::function<bool()> confirmPaste_;
    std::function<void(const QString&)> writeCopy_;
    QFrame* searchBar_ = nullptr;
    QLineEdit* searchInput_ = nullptr;
    QLabel* searchCount_ = nullptr;
    QPushButton *searchOlder_ = nullptr, *searchNewer_ = nullptr;
    QTimer* searchTimer_ = nullptr;
    QTimer* cursorTimer_ = nullptr;
    bool cursorPhase_ = true, preeditCursorVisible_ = true;
    QString preedit_;
    int preeditCursor_ = 0;
    QList<QTextLayout::FormatRange> preeditFormats_;
};
} // namespace nordssh
