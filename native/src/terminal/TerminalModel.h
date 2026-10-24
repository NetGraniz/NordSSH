#pragma once
#include <QByteArray>
#include <QColor>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>
#include <QPointF>
#include <cstddef>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace nordssh {
enum class TerminalCursorShape { Bar, Block, Underline, Hollow };
struct TerminalCell {
    QString text;
    QColor foreground, background;
    int width = 1; // Zero for a wide character's continuation cell.
    bool bold = false, italic = false, underline = false;
    bool strike = false, invisible = false, selected = false;
};
struct TerminalFrame {
    int columns = 0, rows = 0;
    QVector<TerminalCell> cells;
    QColor background;
    bool cursorVisible = false;
    bool cursorBlink = false, cursorWideTail = false, passwordInput = false;
    TerminalCursorShape cursorShape = TerminalCursorShape::Block;
    QColor cursorColor{115, 210, 150};
    bool followingOutput = true, mouseTracking = false;
    int cursorX = 0, cursorY = 0;
};
struct TerminalSearchState {
    std::size_t matches = 0;
    std::optional<std::size_t> selected; // Newest first, zero based.
    bool complete = true, limited = false;
};

// Single-owner GUI-thread adapter. Never expose engine pointers or borrowed
// render buffers to callers. No clipboard, file, URL or process callbacks.
class TerminalModel final {
public:
    static constexpr int MaximumBatch = 65536;
    static constexpr int MaximumPaste = 65536;
    static constexpr int MaximumCopy = 1024 * 1024;
    TerminalModel(int columns = 80, int rows = 24);
    ~TerminalModel();
    TerminalModel(const TerminalModel&) = delete;
    TerminalModel& operator=(const TerminalModel&) = delete;
    void feed(const QByteArray& bytes);
    void resize(int columns, int rows, int cellWidth, int cellHeight);
    void scroll(int lines); // Negative is up; one operation is capped at 10,000 lines.
    void scrollToTop();
    void scrollToBottom();
    // Engine-owned tracked anchors; no borrowed grid refs escape the adapter.
    void beginSelection(QPointF position);
    void extendSelection(QPointF position);
    void endSelection();
    // No selection, empty text or an oversized copy leaves the clipboard alone.
    [[nodiscard]] std::optional<QString> selectionText();
    static constexpr int MaximumSearchBytes = 256;
    static constexpr std::size_t MaximumSearchMatches = 10000;
    // Literal UTF-8 search: ASCII case-insensitive, other scalars byte-exact.
    [[nodiscard]] bool setSearch(const QString& query);
    [[nodiscard]] TerminalSearchState advanceSearch(); // One feed + at most four ticks.
    [[nodiscard]] bool selectSearchMatch(bool older = true); // Only after caught up.
    [[nodiscard]] TerminalFrame frame();
    [[nodiscard]] QByteArray key(const QKeyEvent& event);
    [[nodiscard]] QByteArray mouse(const QMouseEvent& event);
    // One protocol event per detent, capped at 32 per call. No local scrolling.
    [[nodiscard]] QByteArray wheel(const QWheelEvent& event, int steps);
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
