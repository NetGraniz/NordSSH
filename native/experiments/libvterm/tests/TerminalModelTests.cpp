#include "terminal/TerminalModel.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using nordssh::TerminalModel;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Exception, class Action> static void rejects(Action action, const char* message) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error(message);
}

int main() {
    try {
        TerminalModel terminal(24, 80);
        terminal.feed("hello\r\n");
        require(terminal.cell(0, 0).chars[0] == U'h', "ASCII screen");
        // UTF-8 decoder must preserve a split codepoint between transport batches.
        terminal.feed("\xd0");
        terminal.feed("\x96");
        require(terminal.cell(1, 0).chars[0] == U'Ж', "Fragmented Cyrillic UTF-8");
        terminal.feed("e\xcc\x81");
        require(terminal.cell(1, 1).chars[1] == 0x301, "Combining accent");
        terminal.feed("\xe7\x95\x8c");
        require(terminal.cell(1, 2).width == 2, "Wide character width");
        terminal.feed("\x1b[38;2;18;90;120mX");
        require(terminal.cell(1, 4).fg.rgb.red == 18, "Truecolor red");
        terminal.feed("\x1b[?1049hALT\x1b[?1049l");
        require(terminal.cell(0, 0).chars[0] == U'h', "Alternate screen restore");
        terminal.paste("привет");
        require(terminal.takeOutput() == "привет", "Plain paste has no extra Enter or duplicate");
        terminal.feed("\x1b[?2004h");
        terminal.paste("a\nb");
        require(terminal.takeOutput() == "\x1b[200~a\nb\x1b[201~", "Bracketed paste");
        terminal.character(U'c', VTERM_MOD_CTRL);
        require(terminal.takeOutput() == "\x03", "Ctrl+C stays interrupt");
        terminal.feed("\x1b]52;c;?\x07\x1b]52;c;c2VjcmV0\x07");
        require(terminal.takeOutput().empty(), "OSC52 cannot read or write clipboard");
        terminal.resize(40, 100);
        require(terminal.rows() == 40 && terminal.columns() == 100, "Resize");
        rejects<std::invalid_argument>([&] { terminal.resize(100000, 80); }, "Oversized screen accepted");
        rejects<std::length_error>([&] { terminal.feed(std::string(65537, 'x')); }, "Oversized batch accepted");
        rejects<std::length_error>([&] { terminal.paste(std::string(1048576, 'x')); }, "Oversized paste accepted");
        require(terminal.takeOutput().empty(), "Rejected paste emitted partial data");
        for (int i = 0; i < 12000; ++i) terminal.feed("line\r\n");
        require(terminal.scrollbackLines() <= TerminalModel::MaxScrollbackLines, "Scrollback line cap");
        require(terminal.scrollbackBytes() <= TerminalModel::MaxScrollbackBytes, "Scrollback byte cap");
        std::cout << "Terminal core tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
