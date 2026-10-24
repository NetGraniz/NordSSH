#include <ghostty/vt.h>
#include "TerminalModel.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

// Ghostty's C formatter field is named emit; this TU has no Qt signal syntax.
#undef emit

namespace nordssh {
namespace {
void checked(GhosttyResult result) {
    if (result != GHOSTTY_SUCCESS) throw std::runtime_error("Terminal operation failed");
}
void dimensions(int columns, int rows) {
    if (columns < 1 || columns > 512 || rows < 1 || rows > 256)
        throw std::invalid_argument("Terminal dimensions outside limits");
}
QColor color(GhosttyColorRgb value) { return {value.r, value.g, value.b}; }
GhosttyKey physicalKey(const QKeyEvent& event) {
    int key = event.key();
#ifdef Q_OS_WIN
    // Preserve Ctrl+C etc. in a Cyrillic layout without interpreting Windows
    // virtual keys as X11/Wayland keycodes.
    if (event.nativeVirtualKey() >= 0x41 && event.nativeVirtualKey() <= 0x5a)
        key = Qt::Key_A + static_cast<int>(event.nativeVirtualKey() - 0x41);
#endif
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<GhosttyKey>(GHOSTTY_KEY_A + key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + key - Qt::Key_0);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_Return: case Qt::Key_Enter: return GHOSTTY_KEY_ENTER;
    case Qt::Key_Tab: case Qt::Key_Backtab: return GHOSTTY_KEY_TAB;
    case Qt::Key_Backspace: return GHOSTTY_KEY_BACKSPACE;
    case Qt::Key_Escape: return GHOSTTY_KEY_ESCAPE;
    case Qt::Key_Up: return GHOSTTY_KEY_ARROW_UP;
    case Qt::Key_Down: return GHOSTTY_KEY_ARROW_DOWN;
    case Qt::Key_Left: return GHOSTTY_KEY_ARROW_LEFT;
    case Qt::Key_Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case Qt::Key_Home: return GHOSTTY_KEY_HOME;
    case Qt::Key_End: return GHOSTTY_KEY_END;
    case Qt::Key_Insert: return GHOSTTY_KEY_INSERT;
    case Qt::Key_Delete: return GHOSTTY_KEY_DELETE;
    case Qt::Key_PageUp: return GHOSTTY_KEY_PAGE_UP;
    case Qt::Key_PageDown: return GHOSTTY_KEY_PAGE_DOWN;
    case Qt::Key_Space: return GHOSTTY_KEY_SPACE;
    case Qt::Key_QuoteLeft: return GHOSTTY_KEY_BACKQUOTE;
    case Qt::Key_Backslash: return GHOSTTY_KEY_BACKSLASH;
    case Qt::Key_BracketLeft: return GHOSTTY_KEY_BRACKET_LEFT;
    case Qt::Key_BracketRight: return GHOSTTY_KEY_BRACKET_RIGHT;
    case Qt::Key_Comma: return GHOSTTY_KEY_COMMA;
    case Qt::Key_Period: return GHOSTTY_KEY_PERIOD;
    case Qt::Key_Slash: return GHOSTTY_KEY_SLASH;
    case Qt::Key_Semicolon: return GHOSTTY_KEY_SEMICOLON;
    case Qt::Key_Apostrophe: return GHOSTTY_KEY_QUOTE;
    case Qt::Key_Minus: return GHOSTTY_KEY_MINUS;
    case Qt::Key_Equal: return GHOSTTY_KEY_EQUAL;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}
GhosttyMods mouseModifiers(Qt::KeyboardModifiers value) {
    GhosttyMods result = 0;
    if (value.testFlag(Qt::ShiftModifier)) result |= GHOSTTY_MODS_SHIFT;
    if (value.testFlag(Qt::ControlModifier)) result |= GHOSTTY_MODS_CTRL;
    if (value.testFlag(Qt::AltModifier)) result |= GHOSTTY_MODS_ALT;
    if (value.testFlag(Qt::MetaModifier)) result |= GHOSTTY_MODS_SUPER;
    return result;
}
GhosttyMouseButton mouseButton(Qt::MouseButton value) {
    switch (value) {
    case Qt::LeftButton: return GHOSTTY_MOUSE_BUTTON_LEFT;
    case Qt::MiddleButton: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case Qt::RightButton: return GHOSTTY_MOUSE_BUTTON_RIGHT;
    default: return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}
}

struct TerminalModel::Impl {
    GhosttyTerminal terminal{};
    GhosttyRenderState render{};
    GhosttyRenderStateRowIterator rows{};
    GhosttyRenderStateRowCells cells{};
    GhosttyKeyEncoder encoder{};
    GhosttyKeyEvent event{};
    GhosttyMouseEncoder mouseEncoder{};
    GhosttyMouseEvent mouseEvent{};
    GhosttySelectionGesture gesture{};
    GhosttySelectionGestureEvent selectionPress{}, selectionDrag{};
    GhosttySearch search{};
    bool searchDirty = false, searchLimited = false, searchActive = false;
    bool selecting = false;
    int columns = 80, rowCount = 24, cellWidth = 8, cellHeight = 16;
    QThread* owner = QThread::currentThread();
    QByteArray replies;
    bool overflow = false;
    void checkOwner() const {
        if (owner != QThread::currentThread()) throw std::logic_error("Terminal thread ownership violation");
    }
    GhosttyGridRef selectionRef(QPointF position) {
        if (!std::isfinite(position.x()) || !std::isfinite(position.y()))
            throw std::invalid_argument("Nonfinite selection position");
        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        point.value.coordinate.x = static_cast<std::uint16_t>(std::clamp(position.x() / cellWidth, 0.0, double(columns - 1)));
        point.value.coordinate.y = static_cast<std::uint32_t>(std::clamp(position.y() / cellHeight, 0.0, double(rowCount - 1)));
        auto ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
        checked(ghostty_terminal_grid_ref(terminal, point, &ref));
        return ref;
    }
    QByteArray encodeMouse(GhosttyMouseAction action, GhosttyMouseButton button,
                           QPointF position, Qt::KeyboardModifiers mods, bool pressed) {
        if (!std::isfinite(position.x()) || !std::isfinite(position.y()) ||
            std::abs(position.x()) > 1000000 || std::abs(position.y()) > 1000000)
            throw std::invalid_argument("Mouse position outside limits");
        ghostty_mouse_encoder_setopt(mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &pressed);
        ghostty_mouse_event_set_action(mouseEvent, action);
        if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN) ghostty_mouse_event_clear_button(mouseEvent);
        else ghostty_mouse_event_set_button(mouseEvent, button);
        ghostty_mouse_event_set_mods(mouseEvent, mouseModifiers(mods));
        ghostty_mouse_event_set_position(mouseEvent,
            {static_cast<float>(position.x()), static_cast<float>(position.y())});
        std::array<char,128> bytes{};
        std::size_t length = 0;
        checked(ghostty_mouse_encoder_encode(mouseEncoder, mouseEvent, bytes.data(), bytes.size(), &length));
        return {bytes.data(), static_cast<qsizetype>(length)};
    }
    void mouseGeometry() {
        auto size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
        size.screen_width = static_cast<std::uint32_t>(columns * cellWidth);
        size.screen_height = static_cast<std::uint32_t>(rowCount * cellHeight);
        size.cell_width = static_cast<std::uint32_t>(cellWidth);
        size.cell_height = static_cast<std::uint32_t>(cellHeight);
        ghostty_mouse_encoder_setopt(mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    }
    // No exceptions may cross this C ABI boundary. Overflow poisons the adapter
    // instead of dropping protocol bytes and continuing a corrupted session.
    static void write(GhosttyTerminal, void* context, const std::uint8_t* data, std::size_t length) noexcept {
        auto& self = *static_cast<Impl*>(context);
        if (self.overflow) return;
        if (length > 262144U - static_cast<std::size_t>(self.replies.size())) {
            self.overflow = true; return;
        }
        try { self.replies.append(reinterpret_cast<const char*>(data), static_cast<qsizetype>(length)); }
        catch (...) { self.overflow = true; }
    }
    ~Impl() {
        ghostty_search_free(search);
        ghostty_selection_gesture_event_free(selectionDrag);
        ghostty_selection_gesture_event_free(selectionPress);
        ghostty_selection_gesture_free(gesture, terminal);
        ghostty_mouse_event_free(mouseEvent);
        ghostty_mouse_encoder_free(mouseEncoder);
        ghostty_key_event_free(event);
        ghostty_key_encoder_free(encoder);
        ghostty_render_state_row_cells_free(cells);
        ghostty_render_state_row_iterator_free(rows);
        ghostty_render_state_free(render);
        ghostty_terminal_free(terminal);
    }
};

TerminalModel::TerminalModel(int columns, int rows) : impl_(std::make_unique<Impl>()) {
    dimensions(columns, rows);
    auto& state = *impl_;
    checked(ghostty_terminal_new(nullptr, &state.terminal,
        static_cast<std::uint16_t>(columns), static_cast<std::uint16_t>(rows)));
    const std::size_t historyBytes = 4 * 1024 * 1024, historyLines = 10000, apcBytes = 4096;
    const std::uint64_t imageBytes = 0;
    const bool disabled = false;
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &historyBytes));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &historyLines));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_APC_MAX_BYTES, &apcBytes));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &imageBytes));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &disabled));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &disabled));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, nullptr));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ, nullptr));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE, nullptr));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_USERDATA, &state));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
        reinterpret_cast<const void*>(&Impl::write)));
    const GhosttyColorRgb foreground{220, 228, 222}, background{20, 25, 23};
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &foreground));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &background));
    checked(ghostty_render_state_new(nullptr, &state.render));
    checked(ghostty_render_state_row_iterator_new(nullptr, &state.rows));
    checked(ghostty_render_state_row_cells_new(nullptr, &state.cells));
    checked(ghostty_key_encoder_new(nullptr, &state.encoder));
    checked(ghostty_key_event_new(nullptr, &state.event));
    checked(ghostty_mouse_encoder_new(nullptr, &state.mouseEncoder));
    checked(ghostty_mouse_event_new(nullptr, &state.mouseEvent));
    checked(ghostty_selection_gesture_new(nullptr, &state.gesture));
    checked(ghostty_search_new(nullptr, &state.search, state.terminal));
    checked(ghostty_selection_gesture_event_new(nullptr, &state.selectionPress, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS));
    checked(ghostty_selection_gesture_event_new(nullptr, &state.selectionDrag, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG));
    const bool trackCell = true;
    ghostty_mouse_encoder_setopt(state.mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &trackCell);
    state.columns = columns; state.rowCount = rows; state.mouseGeometry();
}
TerminalModel::~TerminalModel() = default;

void TerminalModel::feed(const QByteArray& bytes) {
    auto& state = *impl_; state.checkOwner();
    if (bytes.size() > MaximumBatch) throw std::invalid_argument("Terminal batch exceeds limit");
    GhosttyTerminalScreen before{}, after{};
    checked(ghostty_terminal_get(state.terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &before));
    // Gesture anchors track ordinary output. The engine validates their screen
    // generation before use, including a switch and return within one batch.
    ghostty_terminal_vt_write(state.terminal, reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        static_cast<std::size_t>(bytes.size()));
    state.searchDirty = true;
    checked(ghostty_terminal_get(state.terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &after));
    if (before != after) checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr));
    // This upstream helper resets motion deduplication. Sync on output/mode
    // changes, not on every pointer event in an otherwise unchanged terminal.
    ghostty_mouse_encoder_setopt_from_terminal(state.mouseEncoder, state.terminal);
    bool failed = false;
    checked(ghostty_terminal_get(state.terminal, GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR, &failed));
    if (failed || state.overflow) throw std::runtime_error("Terminal parser or reply limit failure");
}
void TerminalModel::resize(int columns, int rows, int cellWidth, int cellHeight) {
    impl_->checkOwner(); dimensions(columns, rows);
    if (cellWidth < 1 || cellWidth > 1024 || cellHeight < 1 || cellHeight > 1024)
        throw std::invalid_argument("Terminal cell size outside limits");
    endSelection();
    checked(ghostty_terminal_resize(impl_->terminal, static_cast<std::uint16_t>(columns),
        static_cast<std::uint16_t>(rows), static_cast<std::uint32_t>(cellWidth), static_cast<std::uint32_t>(cellHeight)));
    impl_->columns = columns; impl_->rowCount = rows;
    impl_->cellWidth = cellWidth; impl_->cellHeight = cellHeight; impl_->mouseGeometry();
    impl_->searchDirty = true;
}
void TerminalModel::scroll(int lines) {
    impl_->checkOwner();
    if (lines < -10000 || lines > 10000) throw std::invalid_argument("Scroll distance outside limit");
    GhosttyTerminalScrollViewport request{};
    request.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    request.value.delta = lines;
    ghostty_terminal_scroll_viewport(impl_->terminal, request);
}
void TerminalModel::scrollToTop() {
    impl_->checkOwner();
    GhosttyTerminalScrollViewport request{}; request.tag = GHOSTTY_SCROLL_VIEWPORT_TOP;
    ghostty_terminal_scroll_viewport(impl_->terminal, request);
}
void TerminalModel::scrollToBottom() {
    impl_->checkOwner();
    GhosttyTerminalScrollViewport request{}; request.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
    ghostty_terminal_scroll_viewport(impl_->terminal, request);
}
void TerminalModel::beginSelection(QPointF position) {
    auto& state = *impl_; state.checkOwner(); endSelection();
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr));
    auto ref = state.selectionRef(position);
    checked(ghostty_selection_gesture_event_set(state.selectionPress, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref));
    const GhosttySurfacePosition pointer{
        std::clamp(position.x(), 0.0, double(state.columns * state.cellWidth)),
        std::clamp(position.y(), 0.0, double(state.rowCount * state.cellHeight))};
    checked(ghostty_selection_gesture_event_set(state.selectionPress, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &pointer));
    const auto result = ghostty_selection_gesture_event(state.gesture, state.terminal, state.selectionPress, nullptr);
    if (result != GHOSTTY_NO_VALUE) checked(result);
    state.selecting = true;
}
void TerminalModel::extendSelection(QPointF position) {
    auto& state = *impl_; state.checkOwner();
    if (!state.selecting) return;
    auto ref = state.selectionRef(position);
    checked(ghostty_selection_gesture_event_set(state.selectionDrag, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref));
    const GhosttySurfacePosition pointer{
        std::clamp(position.x(), 0.0, double(state.columns * state.cellWidth)),
        std::clamp(position.y(), 0.0, double(state.rowCount * state.cellHeight))};
    checked(ghostty_selection_gesture_event_set(state.selectionDrag, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &pointer));
    const GhosttySelectionGestureGeometry geometry{
        static_cast<std::uint32_t>(state.columns), static_cast<std::uint32_t>(state.cellWidth),
        0, static_cast<std::uint32_t>(state.rowCount * state.cellHeight)};
    checked(ghostty_selection_gesture_event_set(state.selectionDrag, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry));
    auto selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    const auto result = ghostty_selection_gesture_event(state.gesture, state.terminal, state.selectionDrag, &selection);
    if (result == GHOSTTY_NO_VALUE) return;
    checked(result);
    // Snapshot refs expire at the next mutation: install immediately, never cache.
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection));
}
void TerminalModel::endSelection() {
    auto& state = *impl_; state.checkOwner();
    if (state.gesture) ghostty_selection_gesture_reset(state.gesture, state.terminal);
    state.selecting = false;
}
std::optional<QString> TerminalModel::selectionText() {
    auto& state = *impl_; state.checkOwner();
    auto options = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN; options.unwrap = true; options.trim = true;
    std::size_t size = 0;
    const auto result = ghostty_terminal_selection_format_buf(state.terminal, options, nullptr, 0, &size);
    if (result == GHOSTTY_NO_VALUE) return std::nullopt;
    if (result != GHOSTTY_OUT_OF_SPACE) checked(result);
    if (!size || size > MaximumCopy) return std::nullopt;
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    checked(ghostty_terminal_selection_format_buf(state.terminal, options,
        reinterpret_cast<std::uint8_t*>(bytes.data()), size, &size));
    return QString::fromUtf8(bytes.constData(), static_cast<qsizetype>(size));
}
bool TerminalModel::setSearch(const QString& query) {
    auto& state = *impl_; state.checkOwner();
    // Reject before conversion so callers cannot force a large temporary copy.
    if (query.size() > MaximumSearchBytes || !query.isValidUtf16()) return false;
    const auto bytes = query.toUtf8();
    if (bytes.size() > MaximumSearchBytes || std::ranges::any_of(query, [](QChar ch) {
            return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
        })) return false;
    const GhosttyString needle{reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())};
    checked(ghostty_search_set(state.search, GHOSTTY_SEARCH_OPT_NEEDLE, &needle));
    state.searchActive = !bytes.isEmpty(); state.searchLimited = false;
    state.searchDirty = state.searchActive;
    return true;
}
TerminalSearchState TerminalModel::advanceSearch() {
    auto& state = *impl_; state.checkOwner();
    TerminalSearchState result; result.limited = state.searchLimited;
    if (!state.searchActive) return result;
    GhosttySearchStatus status{};
    checked(ghostty_search_get(state.search, GHOSTTY_SEARCH_DATA_STATUS, &status));
    if (state.searchDirty || status == GHOSTTY_SEARCH_STATUS_FEED_REQUIRED) {
        checked(ghostty_search_feed(state.search)); state.searchDirty = false;
        checked(ghostty_search_get(state.search, GHOSTTY_SEARCH_DATA_STATUS, &status));
    }
    for (int i = 0; i < 4 && status == GHOSTTY_SEARCH_STATUS_RUNNING; ++i)
        checked(ghostty_search_tick(state.search, &status));
    checked(ghostty_search_get(state.search, GHOSTTY_SEARCH_DATA_TOTAL_MATCHES, &result.matches));
    if (result.matches > MaximumSearchMatches) {
        checked(ghostty_search_set(state.search, GHOSTTY_SEARCH_OPT_NEEDLE, nullptr));
        state.searchActive = false; state.searchLimited = true;
        result = {}; result.limited = true; return result;
    }
    result.complete = status == GHOSTTY_SEARCH_STATUS_COMPLETE;
    std::size_t index = 0;
    const auto selected = ghostty_search_get(state.search, GHOSTTY_SEARCH_DATA_SELECTED_INDEX, &index);
    if (selected != GHOSTTY_NO_VALUE) { checked(selected); result.selected = index; }
    return result;
}
bool TerminalModel::selectSearchMatch(bool older) {
    auto& state = *impl_; state.checkOwner();
    if (!state.searchActive || state.searchDirty) return false;
    GhosttySearchStatus status{};
    checked(ghostty_search_get(state.search, GHOSTTY_SEARCH_DATA_STATUS, &status));
    if (status != GHOSTTY_SEARCH_STATUS_COMPLETE) return false;
    // The upstream select helper catches up synchronously. Only call it when
    // incremental work is complete and no terminal write/resize intervened.
    const auto selected = ghostty_search_set(state.search,
        older ? GHOSTTY_SEARCH_OPT_SELECT_NEXT : GHOSTTY_SEARCH_OPT_SELECT_PREV, nullptr);
    if (selected == GHOSTTY_NO_VALUE) return false;
    checked(selected);
    auto match = GHOSTTY_INIT_SIZED(GhosttySelection);
    checked(ghostty_search_get(state.search, GHOSTTY_SEARCH_DATA_SELECTED_MATCH, &match));
    checked(ghostty_terminal_set(state.terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &match));
    return true;
}
TerminalFrame TerminalModel::frame() {
    auto& state = *impl_; state.checkOwner();
    checked(ghostty_render_state_update(state.render, state.terminal));
    std::uint16_t columns = 0, rows = 0;
    checked(ghostty_render_state_get(state.render, GHOSTTY_RENDER_STATE_DATA_COLS, &columns));
    checked(ghostty_render_state_get(state.render, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows));
    dimensions(columns, rows);
    TerminalFrame frame;
    checked(ghostty_terminal_get(state.terminal, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &frame.followingOutput));
    checked(ghostty_terminal_get(state.terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &frame.mouseTracking));
    frame.columns = columns; frame.rows = rows;
    frame.cells.resize(columns * rows);
    GhosttyColorRgb foreground{}, background{};
    checked(ghostty_render_state_get(state.render, GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND, &foreground));
    checked(ghostty_render_state_get(state.render, GHOSTTY_RENDER_STATE_DATA_COLOR_BACKGROUND, &background));
    frame.background = color(background);
    auto cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    checked(ghostty_render_state_get(state.render, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor));
    frame.cursorVisible = cursor.visible && cursor.viewport_has_value;
    frame.cursorBlink = cursor.blinking; frame.passwordInput = cursor.password_input;
    switch (cursor.visual_style) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR: frame.cursorShape = TerminalCursorShape::Bar; break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK: frame.cursorShape = TerminalCursorShape::Block; break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE: frame.cursorShape = TerminalCursorShape::Underline; break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW: frame.cursorShape = TerminalCursorShape::Hollow; break;
    default: throw std::runtime_error("Unknown terminal cursor shape");
    }
    GhosttyColorRgb cursorColor{};
    const auto cursorResult = ghostty_terminal_get(state.terminal, GHOSTTY_TERMINAL_DATA_COLOR_CURSOR, &cursorColor);
    if (cursorResult == GHOSTTY_SUCCESS) frame.cursorColor = color(cursorColor);
    else if (cursorResult != GHOSTTY_NO_VALUE) checked(cursorResult);
    if (frame.cursorVisible) {
        frame.cursorX = cursor.viewport_x; frame.cursorY = cursor.viewport_y;
        frame.cursorWideTail = cursor.wide_tail;
    }
    checked(ghostty_render_state_get(state.render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &state.rows));
    int y = 0;
    std::size_t remainingTextBytes = 1024 * 1024;
    std::array<std::uint8_t, 4096> text{};
    while (ghostty_render_state_row_iterator_next(state.rows)) {
        if (y >= rows) throw std::runtime_error("Terminal row overflow");
        checked(ghostty_render_state_row_get(state.rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &state.cells));
        auto selection = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
        const auto selected = ghostty_render_state_row_get(state.rows, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &selection);
        if (selected != GHOSTTY_NO_VALUE) checked(selected);
        int x = 0;
        while (ghostty_render_state_row_cells_next(state.cells)) {
            if (x >= columns) throw std::runtime_error("Terminal column overflow");
            auto& cell = frame.cells[y * columns + x];
            cell.selected = selected == GHOSTTY_SUCCESS && x >= selection.start_x && x <= selection.end_x;
            ++x;
            GhosttyCell raw{};
            GhosttyCellWide wide{};
            checked(ghostty_render_state_row_cells_get(state.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw));
            checked(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide));
            cell.width = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 :
                (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD ? 0 : 1);
            GhosttyBuffer buffer{text.data(), text.size(), 0};
            const auto result = ghostty_render_state_row_cells_get(state.cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer);
            // Extreme combining clusters are not allowed to force unbounded UI allocation.
            if (result == GHOSTTY_OUT_OF_SPACE || (result == GHOSTTY_SUCCESS && buffer.len > remainingTextBytes))
                cell.text = QString(QChar::ReplacementCharacter);
            else {
                checked(result);
                remainingTextBytes -= buffer.len;
                cell.text = QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<qsizetype>(buffer.len));
            }
            GhosttyColorRgb fg = foreground, bg = background;
            const auto fgResult = ghostty_render_state_row_cells_get(state.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
            const auto bgResult = ghostty_render_state_row_cells_get(state.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg);
            if (fgResult != GHOSTTY_INVALID_VALUE) checked(fgResult);
            if (bgResult != GHOSTTY_INVALID_VALUE) checked(bgResult);
            auto style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            checked(ghostty_render_state_row_cells_get(state.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style));
            if (style.inverse) std::swap(fg, bg);
            cell.foreground = color(fg); cell.background = color(bg);
            cell.bold = style.bold; cell.italic = style.italic;
            cell.underline = style.underline != 0; cell.strike = style.strikethrough;
            cell.invisible = style.invisible;
            if (style.faint) cell.foreground.setAlpha(150);
        }
        ++y;
    }
    checked(ghostty_render_state_clean(state.render));
    return frame;
}
QByteArray TerminalModel::key(const QKeyEvent& key) {
    auto& state = *impl_; state.checkOwner();
    // Qt may already translate Ctrl+C/Enter into C0 bytes. The VT encoder
    // expects untransformed text, otherwise it can suppress/duplicate controls.
    auto logicalText = key.text();
    if (std::ranges::any_of(logicalText, [](QChar ch) {
            return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
        })) logicalText.clear();
    const auto text = logicalText.toUtf8();
    if (text.size() > 4096) throw std::invalid_argument("Terminal key text exceeds limit");
    ghostty_key_encoder_setopt_from_terminal(state.encoder, state.terminal);
    ghostty_key_event_set_action(state.event, key.type() == QEvent::KeyRelease ? GHOSTTY_KEY_ACTION_RELEASE :
        (key.isAutoRepeat() ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS));
    ghostty_key_event_set_key(state.event, physicalKey(key));
    GhosttyMods mods = 0;
    if (key.modifiers().testFlag(Qt::ShiftModifier) || key.key() == Qt::Key_Backtab) mods |= GHOSTTY_MODS_SHIFT;
    if (key.modifiers().testFlag(Qt::ControlModifier)) mods |= GHOSTTY_MODS_CTRL;
    if (key.modifiers().testFlag(Qt::AltModifier)) mods |= GHOSTTY_MODS_ALT;
    if (key.modifiers().testFlag(Qt::MetaModifier)) mods |= GHOSTTY_MODS_SUPER;
    ghostty_key_event_set_mods(state.event, mods);
    ghostty_key_event_set_consumed_mods(state.event, 0);
    ghostty_key_event_set_composing(state.event, false);
    ghostty_key_event_set_utf8(state.event, text.isEmpty() ? nullptr : text.constData(), static_cast<std::size_t>(text.size()));
    ghostty_key_event_set_unshifted_codepoint(state.event, 0);
    std::array<char, 8192> output{};
    std::size_t length = 0;
    checked(ghostty_key_encoder_encode(state.encoder, state.event, output.data(), output.size(), &length));
    return {output.data(), static_cast<qsizetype>(length)};
}
QByteArray TerminalModel::mouse(const QMouseEvent& event) {
    auto& state = *impl_; state.checkOwner();
    const bool moving = event.type() == QEvent::MouseMove;
    if (!moving && event.type() != QEvent::MouseButtonPress &&
        event.type() != QEvent::MouseButtonDblClick && event.type() != QEvent::MouseButtonRelease)
        throw std::invalid_argument("Unsupported mouse action");
    auto button = event.button();
    if (moving) {
        button = Qt::NoButton;
        for (const auto candidate : {Qt::LeftButton, Qt::MiddleButton, Qt::RightButton})
            if (event.buttons().testFlag(candidate)) { button = candidate; break; }
    }
    if (button != Qt::NoButton && mouseButton(button) == GHOSTTY_MOUSE_BUTTON_UNKNOWN) return {};
    const auto action = moving ? GHOSTTY_MOUSE_ACTION_MOTION :
        (event.type() == QEvent::MouseButtonRelease ? GHOSTTY_MOUSE_ACTION_RELEASE : GHOSTTY_MOUSE_ACTION_PRESS);
    return state.encodeMouse(action, mouseButton(button), event.position(), event.modifiers(), event.buttons() != Qt::NoButton);
}
QByteArray TerminalModel::wheel(const QWheelEvent& event, int steps) {
    auto& state = *impl_; state.checkOwner();
    if (steps < -32 || steps > 32) throw std::invalid_argument("Mouse wheel batch exceeds limit");
    QByteArray bytes;
    for (int i = 0; i < std::abs(steps); ++i)
        bytes += state.encodeMouse(GHOSTTY_MOUSE_ACTION_PRESS,
            steps > 0 ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE,
            event.position(), event.modifiers(), event.buttons() != Qt::NoButton);
    return bytes;
}
bool TerminalModel::paste(const QByteArray& text, bool confirmed) {
    impl_->checkOwner();
    if (text.size() > MaximumPaste) throw std::invalid_argument("Terminal paste exceeds limit");
    if (!confirmed && !ghostty_paste_is_safe(text.constData(), static_cast<std::size_t>(text.size()))) return false;
    GhosttyTerminalModeConfig mode{GHOSTTY_MODE_BRACKETED_PASTE, false};
    checked(ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_MODE, &mode));
    auto mutableText = text;
    QByteArray encoded(text.size() + 12, Qt::Uninitialized);
    std::size_t length = 0;
    checked(ghostty_paste_encode(mutableText.data(), static_cast<std::size_t>(mutableText.size()), mode.value,
        encoded.data(), static_cast<std::size_t>(encoded.size()), &length));
    Impl::write(impl_->terminal, impl_.get(), reinterpret_cast<const std::uint8_t*>(encoded.constData()), length);
    if (impl_->overflow) throw std::runtime_error("Terminal reply limit exceeded");
    return true;
}
QByteArray TerminalModel::takeReplies() {
    impl_->checkOwner();
    if (impl_->overflow) throw std::runtime_error("Terminal reply limit exceeded");
    return std::exchange(impl_->replies, {});
}
std::size_t TerminalModel::residentPageBytes() const {
    impl_->checkOwner();
    auto usage = GHOSTTY_INIT_SIZED(GhosttyTerminalMemoryUsage);
    checked(ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_MEMORY_USAGE, &usage));
    return static_cast<std::size_t>(usage.primary_resident_bytes + usage.alternate_resident_bytes);
}
} // namespace nordssh
