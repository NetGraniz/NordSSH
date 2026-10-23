#pragma once
#include <QByteArray>
#include <QColor>
#include <QString>
#include <QVector>
#include <memory>

class QKeyEvent;

namespace nordssh {
struct TerminalCell {
    QString text;
    QColor foreground, background;
    int width = 1; // Zero for a wide character's continuation cell.
    bool bold = false, italic = false, underline = false;
    bool strike = false, invisible = false;
};
struct TerminalFrame {
    int columns = 0, rows = 0;
    QVector<TerminalCell> cells;
    QColor background;
    bool cursorVisible = false;
    int cursorX = 0, cursorY = 0;
};

// Single-owner GUI-thread adapter. Never expose engine pointers or borrowed
// render buffers to callers. No clipboard, file, URL or process callbacks.
class TerminalModel final {
public:
    static constexpr int MaximumBatch = 65536;
    static constexpr int MaximumPaste = 65536;
    TerminalModel(int columns = 80, int rows = 24);
    ~TerminalModel();
    TerminalModel(const TerminalModel&) = delete;
    TerminalModel& operator=(const TerminalModel&) = delete;
    void feed(const QByteArray& bytes);
    void resize(int columns, int rows, int cellWidth, int cellHeight);
    [[nodiscard]] TerminalFrame frame();
    [[nodiscard]] QByteArray key(const QKeyEvent& event);
    // Refusal writes nothing. Confirmation must apply to this captured text,
    // not a second clipboard read after a nested dialog has run.
    [[nodiscard]] bool paste(const QByteArray& text, bool confirmed = false);
    [[nodiscard]] QByteArray takeReplies();
    [[nodiscard]] std::size_t residentPageBytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace nordssh
