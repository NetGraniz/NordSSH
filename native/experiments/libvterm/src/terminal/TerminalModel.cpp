#include "TerminalModel.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nordssh {

void TerminalModel::checkDimensions(int rows, int columns) {
    if (rows < 1 || rows > MaxRows || columns < 1 || columns > MaxColumns)
        throw std::invalid_argument("Terminal dimensions outside supported bounds");
}

TerminalModel::TerminalModel(int rows, int columns)
    : terminal_(nullptr, &vterm_free), rows_(rows), columns_(columns) {
    checkDimensions(rows, columns);
    terminal_.reset(vterm_new(rows, columns));
    if (!terminal_) throw std::bad_alloc();
    vterm_set_utf8(terminal_.get(), 1);
    vterm_output_set_callback(terminal_.get(), &outputCallback, this);
    screen_ = vterm_obtain_screen(terminal_.get());
    static const VTermScreenCallbacks callbacks = [] {
        VTermScreenCallbacks result{};
        result.sb_pushline = &pushLine;
        result.sb_popline = &popLine;
        return result;
    }();
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_reset(screen_, 1);
}

TerminalModel::~TerminalModel() = default;

void TerminalModel::feed(std::string_view bytes) {
    if (bytes.size() > MaxFeedBytes) throw std::length_error("Terminal feed exceeds batch limit");
    if (!bytes.empty()) vterm_input_write(terminal_.get(), bytes.data(), bytes.size());
    vterm_screen_flush_damage(screen_);
}

void TerminalModel::resize(int rows, int columns) {
    checkDimensions(rows, columns);
    vterm_set_size(terminal_.get(), rows, columns);
    rows_ = rows;
    columns_ = columns;
    vterm_screen_flush_damage(screen_);
}

void TerminalModel::key(VTermKey keyValue, VTermModifier modifiers) {
    vterm_keyboard_key(terminal_.get(), keyValue, modifiers);
}

void TerminalModel::character(char32_t value, VTermModifier modifiers) {
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        throw std::invalid_argument("Invalid Unicode scalar");
    vterm_keyboard_unichar(terminal_.get(), static_cast<uint32_t>(value), modifiers);
}

void TerminalModel::paste(std::string_view utf8) {
    // Atomic admission: never send a partial bracketed paste when the queue is full.
    constexpr std::size_t framingBytes = 12;
    if (utf8.size() > MaxOutputBytes - framingBytes ||
        output_.size() > MaxOutputBytes - framingBytes - utf8.size())
        throw std::length_error("Paste exceeds pending input limit");
    vterm_keyboard_start_paste(terminal_.get());
    appendOutput(utf8);
    vterm_keyboard_end_paste(terminal_.get());
}

void TerminalModel::mouseMove(int row, int column, VTermModifier modifiers) {
    if (row < 0 || row >= rows_ || column < 0 || column >= columns_) return;
    vterm_mouse_move(terminal_.get(), row, column, modifiers);
}

void TerminalModel::mouseButton(int button, bool pressed, VTermModifier modifiers) {
    if (button < 1 || button > 5) return;
    vterm_mouse_button(terminal_.get(), button, pressed ? 1 : 0, modifiers);
}

void TerminalModel::appendOutput(std::string_view bytes) noexcept {
    if (bytes.size() > MaxOutputBytes - output_.size()) { overflow_ = true; return; }
    try { output_.append(bytes); }
    catch (...) { overflow_ = true; }
}

void TerminalModel::outputCallback(const char* bytes, std::size_t length, void* owner) noexcept {
    static_cast<TerminalModel*>(owner)->appendOutput({bytes, length});
}

std::string TerminalModel::takeOutput() { return std::exchange(output_, {}); }

int TerminalModel::pushLine(int columns, const VTermScreenCell* cells, void* owner) noexcept {
    auto& self = *static_cast<TerminalModel*>(owner);
    if (columns < 1 || columns > MaxColumns) return 0;
    const auto bytes = static_cast<std::size_t>(columns) * sizeof(VTermScreenCell);
    while (!self.history_.empty() && (self.history_.size() >= MaxScrollbackLines ||
           self.historyBytes_ + bytes > MaxScrollbackBytes)) {
        self.historyBytes_ -= self.history_.front().size() * sizeof(VTermScreenCell);
        self.history_.pop_front();
    }
    try {
        self.history_.emplace_back(cells, cells + columns);
        self.historyBytes_ += bytes;
        return 1;
    } catch (...) { return 0; } // No C++ exception crosses the C callback boundary.
}

int TerminalModel::popLine(int columns, VTermScreenCell* cells, void* owner) noexcept {
    auto& self = *static_cast<TerminalModel*>(owner);
    if (self.history_.empty() || columns < 1 || columns > MaxColumns) return 0;
    const auto& line = self.history_.back();
    std::fill(cells, cells + columns, VTermScreenCell{});
    std::copy_n(line.begin(), std::min(line.size(), static_cast<std::size_t>(columns)), cells);
    self.historyBytes_ -= line.size() * sizeof(VTermScreenCell);
    self.history_.pop_back();
    return 1;
}

VTermScreenCell TerminalModel::cell(int row, int column) const {
    if (row < 0 || row >= rows_ || column < 0 || column >= columns_)
        throw std::out_of_range("Terminal cell outside screen");
    VTermScreenCell result{};
    vterm_screen_get_cell(screen_, {row, column}, &result);
    return result;
}

VTermPos TerminalModel::cursor() const {
    VTermPos result{};
    vterm_state_get_cursorpos(vterm_obtain_state(terminal_.get()), &result);
    return result;
}

const std::vector<VTermScreenCell>& TerminalModel::historyLine(std::size_t index) const {
    return history_.at(index);
}

} // namespace nordssh
