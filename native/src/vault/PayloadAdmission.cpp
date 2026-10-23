#include "PayloadAdmission.h"
#include "VaultFormat.h"
#include <array>
#include <cstdint>
#include <new>
#include <set>
#include <string>

namespace nordssh::vault {
namespace {
enum class Kind { Object, Array, Integer, String, Other };
struct Value { Kind kind; std::int64_t integer = 0; std::size_t children = 0; };
class Scanner final {
public:
    explicit Scanner(std::string_view input) : input_(input) {}
    PayloadCheck run() {
        if (input_.empty() || input_.size() > MaxPlaintextBytes) fail(PayloadStatus::ResourceLimit);
        const auto root = value(1, true);
        whitespace();
        if (at_ != input_.size()) fail(PayloadStatus::Syntax);
        if (root.kind != Kind::Object) fail(PayloadStatus::RootSchema);
        return {PayloadStatus::StructureAccepted, nodes_, records_};
    }
private:
    [[noreturn]] static void fail(PayloadStatus reason) { throw reason; }
    char peek() const { return at_ < input_.size() ? input_[at_] : '\0'; }
    void whitespace() {
        while (at_ < input_.size() && (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n')) ++at_;
    }
    bool take(char ch) { if (at_ < input_.size() && peek() == ch) { ++at_; return true; } return false; }
    void expect(char ch) { if (!take(ch)) fail(PayloadStatus::Syntax); }
    static bool digit(char ch) { return ch >= '0' && ch <= '9'; }
    std::uint32_t hex4() {
        std::uint32_t scalar = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_ == input_.size()) fail(PayloadStatus::Syntax);
            const char ch = input_[at_++];
            unsigned nibble = 0;
            if (digit(ch)) nibble = static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') nibble = static_cast<unsigned>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') nibble = static_cast<unsigned>(ch - 'A' + 10);
            else fail(PayloadStatus::Syntax);
            scalar = (scalar << 4) | nibble;
        }
        return scalar;
    }
    std::uint32_t utf8() {
        const auto lead = static_cast<unsigned char>(input_[at_++]);
        std::uint32_t scalar = lead, minimum = 0;
        unsigned remaining = 0;
        if (lead <= 0x7f) return scalar;
        if (lead >= 0xc2 && lead <= 0xdf) { scalar &= 0x1f; remaining = 1; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { scalar &= 0x0f; remaining = 2; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { scalar &= 0x07; remaining = 3; minimum = 0x10000; }
        else fail(PayloadStatus::InvalidUtf8);
        if (input_.size() - at_ < remaining) fail(PayloadStatus::InvalidUtf8);
        for (unsigned i = 0; i < remaining; ++i) {
            const auto next = static_cast<unsigned char>(input_[at_++]);
            if ((next & 0xc0) != 0x80) fail(PayloadStatus::InvalidUtf8);
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) fail(PayloadStatus::InvalidUtf8);
        return scalar;
    }
    std::string string(bool key) {
        expect('"');
        std::string decodedKey;
        std::size_t decodedBytes = 0;
        while (!take('"')) {
            if (at_ == input_.size()) fail(PayloadStatus::Syntax);
            std::uint32_t scalar = 0;
            if (take('\\')) {
                if (at_ == input_.size()) fail(PayloadStatus::Syntax);
                switch (input_[at_++]) {
                case '"': scalar = '"'; break;
                case '\\': scalar = '\\'; break;
                case '/': scalar = '/'; break;
                case 'b': scalar = '\b'; break;
                case 'f': scalar = '\f'; break;
                case 'n': scalar = '\n'; break;
                case 'r': scalar = '\r'; break;
                case 't': scalar = '\t'; break;
                case 'u':
                    scalar = hex4();
                    if (scalar >= 0xd800 && scalar <= 0xdbff) {
                        expect('\\'); expect('u'); const auto low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail(PayloadStatus::Syntax);
                        scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
                    } else if (scalar >= 0xdc00 && scalar <= 0xdfff) fail(PayloadStatus::Syntax);
                    break;
                default: fail(PayloadStatus::Syntax);
                }
            } else {
                scalar = utf8();
                if (scalar < 0x20) fail(PayloadStatus::Syntax);
            }
            std::array<char,4> bytes{};
            std::size_t count = 0;
            if (scalar < 0x80) { bytes[0] = static_cast<char>(scalar); count = 1; }
            else if (scalar < 0x800) {
                bytes[0] = static_cast<char>(0xc0 | (scalar >> 6)); bytes[1] = static_cast<char>(0x80 | (scalar & 0x3f)); count = 2;
            } else if (scalar < 0x10000) {
                bytes[0] = static_cast<char>(0xe0 | (scalar >> 12)); bytes[1] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3f));
                bytes[2] = static_cast<char>(0x80 | (scalar & 0x3f)); count = 3;
            } else {
                bytes[0] = static_cast<char>(0xf0 | (scalar >> 18)); bytes[1] = static_cast<char>(0x80 | ((scalar >> 12) & 0x3f));
                bytes[2] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)); bytes[3] = static_cast<char>(0x80 | (scalar & 0x3f)); count = 4;
            }
            const std::size_t limit = key ? 128 : 1048576;
            if (count > limit - decodedBytes) fail(PayloadStatus::ResourceLimit);
            decodedBytes += count;
            if (key) decodedKey.append(bytes.data(), count);
        }
        return decodedKey;
    }
    Value number() {
        const auto start = at_;
        const bool negative = take('-');
        if (!digit(peek())) fail(PayloadStatus::Syntax);
        const bool zero = peek() == '0';
        std::uint64_t magnitude = 0;
        do {
            const auto part = static_cast<unsigned>(input_[at_++] - '0');
            if (at_ - start > 20 || magnitude > (9007199254740991ULL - part) / 10) fail(PayloadStatus::ResourceLimit);
            magnitude = magnitude * 10 + part;
            if (zero && digit(peek())) fail(PayloadStatus::Syntax);
        } while (digit(peek()));
        const auto integer = static_cast<std::int64_t>(magnitude);
        return {Kind::Integer, negative ? -integer : integer};
    }
    Value object(unsigned depth, bool root) {
        if (depth > 16) fail(PayloadStatus::ResourceLimit);
        expect('{'); whitespace();
        std::set<std::string> names;
        unsigned fields = 0;
        if (!take('}')) {
            do {
                whitespace();
                if (names.size() >= 256) fail(PayloadStatus::ResourceLimit);
                const auto name = string(true);
                if (!names.insert(name).second) fail(PayloadStatus::DuplicateKey);
                whitespace(); expect(':');
                int rootField = -1;
                if (root) {
                    constexpr std::array<std::string_view,7> allowed{"schema","hosts","groups","credentials","known_hosts","snippets","settings"};
                    for (std::size_t i = 0; i < allowed.size(); ++i) if (name == allowed[i]) rootField = static_cast<int>(i);
                    if (rootField < 0) fail(PayloadStatus::RootSchema);
                    fields |= 1U << static_cast<unsigned>(rootField);
                }
                const auto member = value(depth + 1, false, rootField >= 1 && rootField <= 5);
                if (rootField == 0) {
                    if (member.kind != Kind::Integer) fail(PayloadStatus::RootSchema);
                    if (member.integer != 1) fail(PayloadStatus::UnsupportedSchema);
                } else if (rootField >= 1) {
                    if (member.kind != (rootField == 6 ? Kind::Object : Kind::Array)) fail(PayloadStatus::RootSchema);
                    if (member.children != 0) records_ = true;
                }
                whitespace();
            } while (take(','));
            expect('}');
        }
        if (root && fields != 0x7f) fail(PayloadStatus::RootSchema);
        return {Kind::Object,0,names.size()};
    }
    Value array(unsigned depth, bool objectsOnly) {
        if (depth > 16) fail(PayloadStatus::ResourceLimit);
        expect('['); whitespace(); std::size_t count = 0;
        if (!take(']')) {
            do {
                if (count == 10000) fail(PayloadStatus::ResourceLimit);
                const auto item = value(depth + 1);
                if (objectsOnly && item.kind != Kind::Object) fail(PayloadStatus::RootSchema);
                ++count; whitespace();
            } while (take(','));
            expect(']');
        }
        return {Kind::Array,0,count};
    }
    Value value(unsigned depth, bool root=false, bool objectsOnly=false) {
        if (++nodes_ > 100000) fail(PayloadStatus::ResourceLimit);
        whitespace();
        switch (peek()) {
        case '{': return object(depth,root);
        case '[': return array(depth,objectsOnly);
        case '"': static_cast<void>(string(false)); return {Kind::String};
        default: break;
        }
        if (peek() == '-' || digit(peek())) return number();
        for (const auto literal : {std::string_view("true"),std::string_view("false"),std::string_view("null")}) {
            if (input_.substr(at_).starts_with(literal)) { at_ += literal.size(); return {Kind::Other}; }
        }
        fail(PayloadStatus::Syntax);
    }
    std::string_view input_;
    std::size_t at_ = 0, nodes_ = 0;
    bool records_ = false;
};
}
PayloadCheck inspectPayloadStructure(std::string_view authenticatedUtf8) {
    try { return Scanner(authenticatedUtf8).run(); }
    catch (PayloadStatus status) { return {status}; }
    catch (const std::bad_alloc&) { return {PayloadStatus::ResourceLimit}; }
}
} // namespace nordssh::vault
