// Engineering ABI/engine test, not a QWidget renderer or user-facing terminal.
#include <ghostty/vt.h>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

static void require(bool result,const char* message) { if(!result) throw std::runtime_error(message); }
static void checked(GhosttyResult result) { require(result==GHOSTTY_SUCCESS,"Ghostty C API call failed"); }
struct Terminal {
    GhosttyTerminal value{};
    ~Terminal(){ghostty_terminal_free(value);}
};
static void feed(GhosttyTerminal terminal,std::string_view bytes) {
    require(bytes.size()<=65536,"Feed exceeds engineering batch cap");
    ghostty_terminal_vt_write(terminal,reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());
    bool failed=false;
    checked(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed));
    require(!failed,"VT processing reported a semantic error");
}
static std::string text(GhosttyTerminal terminal) {
    auto options=GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
    options.emit=GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.trim=true;
    GhosttyFormatter formatter{};
    checked(ghostty_formatter_terminal_new(nullptr,&formatter,terminal,options));
    struct Cleanup {GhosttyFormatter value; ~Cleanup(){ghostty_formatter_free(value);}} cleanup{formatter};
    std::array<std::uint8_t,4096> output{};
    std::size_t length=0;
    checked(ghostty_formatter_format_buf(formatter,output.data(),output.size(),&length));
    return {reinterpret_cast<const char*>(output.data()),length};
}
int main() {
    try {
        Terminal terminal;
        checked(ghostty_terminal_new(nullptr,&terminal.value,80,24));
        const std::size_t history=1024*1024,lines=1000;
        checked(ghostty_terminal_set(terminal.value,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,&history));
        checked(ghostty_terminal_set(terminal.value,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,&lines));
        checked(ghostty_terminal_set(terminal.value,GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ,nullptr));
        checked(ghostty_terminal_set(terminal.value,GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,nullptr));
        feed(terminal.value,"Hello ");
        feed(terminal.value,"\xd0"); feed(terminal.value,"\x96");
        feed(terminal.value,"e\xcc\x81\xe7\x95\x8c");
        require(text(terminal.value).find("Hello \xd0\x96" "e\xcc\x81\xe7\x95\x8c")!=std::string::npos,"C++ ABI UTF8/combining/wide baseline");
        feed(terminal.value,"\x1b[?1049hALTERNATE\x1b[?1049l");
        require(text(terminal.value).find("Hello")!=std::string::npos,"Alternate screen restore");
        checked(ghostty_terminal_resize(terminal.value,100,40,9,18));
        feed(terminal.value,"\x1b]52;c;?\x07\x1b]52;c;c3ludGhldGlj\x07");
        for(int i=0;i<10000;++i) feed(terminal.value,"bounded history\r\n");
        auto usage=GHOSTTY_INIT_SIZED(GhosttyTerminalMemoryUsage);
        checked(ghostty_terminal_get(terminal.value,GHOSTTY_TERMINAL_DATA_MEMORY_USAGE,&usage));
        require(usage.primary_resident_bytes+usage.alternate_resident_bytes<8*1024*1024,"Engineering page memory exceeds budget");
        std::cout<<"Ghostty C++ ABI/VT baseline passed; no renderer, OS clipboard or total RSS assertion\n";
        return EXIT_SUCCESS;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return EXIT_FAILURE;}
}
