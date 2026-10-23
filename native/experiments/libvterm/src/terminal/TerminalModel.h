#pragma once

#include <vterm.h>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nordssh {

// EXPERIMENT ONLY: upstream parser failed security review; do not use with SSH.
// Single-owner model. The GUI owns it; no libssh/worker thread may access it.
// It has no clipboard, shell, filesystem or URL-opening capability.
class TerminalModel final {
public:
    static constexpr std::size_t MaxFeedBytes = 64 * 1024;
    static constexpr std::size_t MaxOutputBytes = 1024 * 1024;
    static constexpr std::size_t MaxScrollbackBytes = 32 * 1024 * 1024;
    static constexpr std::size_t MaxScrollbackLines = 10000;
    static constexpr int MaxRows = 512;
    static constexpr int MaxColumns = 1024;

    explicit TerminalModel(int rows = 24, int columns = 80);
    ~TerminalModel();
    TerminalModel(const TerminalModel&) = delete;
    TerminalModel& operator=(const TerminalModel&) = delete;

    void feed(std::string_view bytes);
    void resize(int rows, int columns);
    void key(VTermKey key, VTermModifier modifiers = VTERM_MOD_NONE);
    void character(char32_t character, VTermModifier modifiers = VTERM_MOD_NONE);
    void paste(std::string_view utf8);
    void mouseMove(int row, int column, VTermModifier modifiers = VTERM_MOD_NONE);
    void mouseButton(int button, bool pressed, VTermModifier modifiers = VTERM_MOD_NONE);
    [[nodiscard]] std::string takeOutput();
    [[nodiscard]] VTermScreenCell cell(int row, int column) const;
    [[nodiscard]] VTermPos cursor() const;
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] int columns() const noexcept { return columns_; }
    [[nodiscard]] std::size_t scrollbackLines() const noexcept { return history_.size(); }
    [[nodiscard]] std::size_t scrollbackBytes() const noexcept { return historyBytes_; }
    [[nodiscard]] const std::vector<VTermScreenCell>& historyLine(std::size_t index) const;
    [[nodiscard]] bool outputOverflowed() const noexcept { return overflow_; }

private:
    using Line = std::vector<VTermScreenCell>;
    static void outputCallback(const char* bytes, std::size_t length, void* owner) noexcept;
    static int pushLine(int columns, const VTermScreenCell* cells, void* owner) noexcept;
    static int popLine(int columns, VTermScreenCell* cells, void* owner) noexcept;
    static void checkDimensions(int rows, int columns);
    void appendOutput(std::string_view bytes) noexcept;

    std::unique_ptr<VTerm, decltype(&vterm_free)> terminal_;
    VTermScreen* screen_{};
    int rows_;
    int columns_;
    std::deque<Line> history_;
    std::size_t historyBytes_{};
    std::string output_;
    bool overflow_{};
};

} // namespace nordssh
