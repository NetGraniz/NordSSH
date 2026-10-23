#pragma once
#include "TerminalModel.h"
#include <QWidget>
#include <functional>

namespace nordssh {
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
    [[nodiscard]] TerminalFrame snapshot();
    QSize sizeHint() const override;
protected:
    bool event(QEvent*) override;
    void changeEvent(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
private:
    void paste();
    void flush();
    void fail() noexcept;
    void updateMetrics();
    void refreshGeometry();
    TerminalModel model_;
    TerminalFrame frame_;
    int cellWidth_ = 9, cellHeight_ = 18, baseline_ = 14;
    bool failed_ = false;
    std::function<void(const QByteArray&)> send_;
    std::function<void(int, int)> resize_;
    std::function<void()> failure_;
    std::function<QString()> readPaste_;
    std::function<bool()> confirmPaste_;
};
} // namespace nordssh
