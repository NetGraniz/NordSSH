#include "VaultDocument.h"
#include "PayloadAdmission.h"
#include "ssh/HostResolver.h"
#include "ssh/KeyMaterial.h"
#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <cstring>

namespace nordssh::vault {
namespace {
[[noreturn]] void bad() { throw DocumentError(); }
std::string_view view(const SecretBytes& bytes) {
    if (bytes.bytes().empty()) return {};
    return {reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()};
}
SecretBytes copy(std::string_view text) {
    SecretBytes result(text.size());
    if (!text.empty()) std::memcpy(result.bytes().data(), text.data(), text.size());
    return result;
}
struct Node {
    enum Kind { Object, Array, String, Integer, Boolean, Null } kind = Null;
    std::map<std::string, Node> members;
    std::vector<Node> items;
    SecretBytes storage{0};
    std::size_t length = 0;
    std::int64_t integer = 0;
    std::string_view text() const { if (kind != String) bad(); return view(storage).substr(0, length); }
};
class Parser {
public:
    explicit Parser(std::string_view data) : data_(data) {}
    Node parse() {
        space(); const char c = data_[at_];
        Node node;
        if (c == '{') {
            node.kind = Node::Object; ++at_; space();
            if (data_[at_] != '}') do {
                space(); auto key = string(); space(); ++at_;
                node.members.emplace(std::string(key.text()), parse()); space();
            } while (take(','));
            ++at_;
        } else if (c == '[') {
            node.kind = Node::Array; ++at_; space();
            if (data_[at_] != ']') do { node.items.push_back(parse()); space(); } while (take(','));
            ++at_;
        } else if (c == '"') node = string();
        else if (c == 't' || c == 'f') { node.kind = Node::Boolean; node.integer = c == 't'; at_ += c == 't' ? 4 : 5; }
        else if (c == 'n') { at_ += 4; }
        else {
            node.kind = Node::Integer;
            const auto start = at_++;
            while (at_ < data_.size() && data_[at_] >= '0' && data_[at_] <= '9') ++at_;
            const auto result = std::from_chars(data_.data() + start, data_.data() + at_, node.integer);
            if (result.ec != std::errc{}) bad();
        }
        return node;
    }
private:
    void space() { while (at_ < data_.size() && (data_[at_] == ' ' || data_[at_] == '\r' || data_[at_] == '\n' || data_[at_] == '\t')) ++at_; }
    bool take(char c) { if (at_ < data_.size() && data_[at_] == c) { ++at_; return true; } return false; }
    unsigned hex() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) { const char c = data_[at_++]; value = (value << 4) | static_cast<unsigned>(c <= '9' ? c - '0' : c <= 'F' ? c - 'A' + 10 : c - 'a' + 10); }
        return value;
    }
    Node string() {
        ++at_; const auto start = at_;
        auto end = at_;
        while (data_[end] != '"') { if (data_[end] == '\\') ++end; ++end; }
        Node node; node.kind = Node::String; node.storage = SecretBytes(end - start);
        auto append = [&](unsigned c) { node.storage.bytes()[node.length++] = std::byte(c); };
        while (at_ < end) {
            unsigned c = static_cast<unsigned char>(data_[at_++]);
            if (c != '\\') { append(c); continue; }
            switch (data_[at_++]) {
            case '"': append('"'); break; case '\\': append('\\'); break; case '/': append('/'); break;
            case 'b': append('\b'); break; case 'f': append('\f'); break; case 'n': append('\n'); break;
            case 'r': append('\r'); break; case 't': append('\t'); break;
            case 'u':
                c = hex();
                if (c >= 0xd800 && c <= 0xdbff) { at_ += 2; c = 0x10000 + ((c - 0xd800) << 10) + hex() - 0xdc00; }
                if (c < 0x80) append(c);
                else if (c < 0x800) { append(0xc0 | (c >> 6)); append(0x80 | (c & 63)); }
                else if (c < 0x10000) { append(0xe0 | (c >> 12)); append(0x80 | ((c >> 6) & 63)); append(0x80 | (c & 63)); }
                else { append(0xf0 | (c >> 18)); append(0x80 | ((c >> 12) & 63)); append(0x80 | ((c >> 6) & 63)); append(0x80 | (c & 63)); }
                break;
            default: bad();
            }
        }
        ++at_; return node;
    }
    std::string_view data_; std::size_t at_ = 0;
};
const Node& field(const Node& node, const char* key) {
    if (node.kind != Node::Object) bad();
    const auto found = node.members.find(key); if (found == node.members.end()) bad(); return found->second;
}
void fields(const Node& node, std::initializer_list<const char*> keys) {
    if (node.kind != Node::Object || node.members.size() != keys.size()) bad();
    for (const auto key : keys) (void)field(node, key);
}
std::string text(const Node& node, const char* key, std::size_t maximum, bool empty = false, bool multiline = false) {
    const auto value = field(node, key).text();
    if ((!empty && value.empty()) || value.size() > maximum || std::ranges::any_of(value, [=](unsigned char c) {
        return c == 0 || c == 127 || (c < 32 && !(multiline && (c == '\n' || c == '\r' || c == '\t')));
    })) bad();
    return std::string(value);
}
unsigned number(const Node& node, const char* key, unsigned low, unsigned high) {
    const auto& value = field(node, key);
    if (value.kind != Node::Integer || value.integer < low || value.integer > high) bad();
    return static_cast<unsigned>(value.integer);
}
bool boolean(const Node& node, const char* key) { const auto& value = field(node, key); if (value.kind != Node::Boolean) bad(); return value.integer != 0; }
const std::vector<Node>& collection(const Node& root, const char* key, std::size_t maximum) {
    const auto& value = field(root, key); if (value.kind != Node::Array || value.items.size() > maximum) bad(); return value.items;
}
bool uuid(std::string_view value) {
    if (value.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else { if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false; nonzero = nonzero || value[i] != '0'; }
    }
    return nonzero;
}
void hostName(const std::string& host) { const auto canonical = ssh::HostResolver::canonicalHost(host); if (!canonical || *canonical != host) bad(); }
void references(const VaultDocument& doc) {
    std::map<std::string, const GroupRecord*> groups;
    std::map<std::string, const HostRecord*> hosts;
    std::map<std::string, const CredentialRecord*> credentials;
    std::set<std::string> ids;
    const auto unique = [&](const auto& record) { if (!uuid(record.id) || !ids.insert(record.id).second) bad(); };
    for (const auto& group : doc.groups) { unique(group); groups[group.id] = &group; }
    for (const auto& credential : doc.credentials) { unique(credential); credentials[credential.id] = &credential; }
    for (const auto& host : doc.hosts) { unique(host); hosts[host.id] = &host; }
    for (const auto& snippet : doc.snippets) unique(snippet);
    for (const auto& group : doc.groups) {
        auto parent = group.parent; unsigned depth = 0;
        while (!parent.empty()) { if (++depth > 16 || parent == group.id || !groups.contains(parent)) bad(); parent = groups.at(parent)->parent; }
    }
    for (const auto& host : doc.hosts) {
        if (!host.group.empty() && !groups.contains(host.group)) bad();
        if (!host.jump.empty() && (!hosts.contains(host.jump) || host.jump == host.id || !hosts.at(host.jump)->jump.empty())) bad();
        if (!host.credential.empty()) {
            if (!credentials.contains(host.credential)) bad();
            const auto& c = *credentials.at(host.credential);
            if (c.host != host.host || c.port != host.port || c.user != host.user) bad();
        }
    }
}
class Writer {
public:
    void raw(std::string_view value) { if (value.size() > buffer_.bytes().size() - at_) bad(); std::memcpy(buffer_.bytes().data() + at_, value.data(), value.size()); at_ += value.size(); }
    void string(std::string_view value) {
        raw("\""); constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : value) {
            const auto c = static_cast<unsigned char>(byte);
            if (c == '"') raw("\\\""); else if (c == '\\') raw("\\\\");
            else if (c < 32) { const std::array<char, 6> escaped{'\\','u','0','0',hex[c >> 4],hex[c & 15]}; raw({escaped.data(), escaped.size()}); }
            else raw({&byte, 1});
        }
        raw("\"");
    }
    void member(std::string_view key, std::string_view value) { string(key); raw(":"); string(value); }
    void number(std::string_view key, unsigned value) { string(key); raw(":"); raw(std::to_string(value)); }
    SecretBytes finish() { SecretBytes result(at_); std::ranges::copy(buffer_.bytes().first(at_), result.bytes().begin()); return result; }
private: SecretBytes buffer_{MaxPlaintextBytes}; std::size_t at_ = 0;
};
}
VaultDocument parseDocument(std::span<const std::byte> payload) {
    const std::string_view json(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (inspectPayloadStructure(json).status != PayloadStatus::StructureAccepted) bad();
    auto root = Parser(json).parse(); VaultDocument doc;
    for (const auto& n : collection(root, "groups", 256)) {
        fields(n, {"id","name","parent"}); doc.groups.push_back({text(n,"id",36),text(n,"name",256),text(n,"parent",36,true)});
    }
    for (const auto& n : collection(root, "credentials", 1000)) {
        fields(n,{"id","kind","host","port","user","secret","passphrase","certificate"});
        CredentialRecord c; c.id=text(n,"id",36); c.kind=text(n,"kind",16); c.host=text(n,"host",253); c.port=number(n,"port",1,65535);
        c.user=text(n,"user",128); c.certificate=text(n,"certificate",65536,true,true); hostName(c.host);
        const auto value=field(n,"secret").text(), phrase=field(n,"passphrase").text();
        if (value.find('\0') != value.npos || phrase.find('\0') != phrase.npos || phrase.size()>4096) bad();
        if (c.kind=="password") { if(value.empty()||value.size()>4096||!phrase.empty()||!c.certificate.empty()) bad(); }
        else if(c.kind=="key") { try { (void)ssh::admitOpenSshKey(value); } catch(...) { bad(); } }
        else if(c.kind=="interactive") { if(!value.empty()||!phrase.empty()||!c.certificate.empty()) bad(); }
        else bad();
        c.secret=copy(value); c.passphrase=copy(phrase); doc.credentials.push_back(std::move(c));
    }
    for(const auto& n:collection(root,"hosts",1000)) {
        fields(n,{"id","name","host","port","user","group","credential","jump","favorite"});
        HostRecord h; h.id=text(n,"id",36); h.name=text(n,"name",256); h.host=text(n,"host",253); hostName(h.host);
        h.user=text(n,"user",128); h.port=number(n,"port",1,65535); h.group=text(n,"group",36,true); h.credential=text(n,"credential",36,true);
        h.jump=text(n,"jump",36,true); h.favorite=boolean(n,"favorite"); doc.hosts.push_back(std::move(h));
    }
    std::set<std::pair<std::string,unsigned>> pins;
    for(const auto& n:collection(root,"known_hosts",1000)) {
        fields(n,{"host","port","algorithm","sha256"}); TrustRecord pin;
        pin.host=text(n,"host",253); hostName(pin.host); pin.port=number(n,"port",1,65535); pin.algorithm=text(n,"algorithm",128); pin.sha256=text(n,"sha256",64);
        if(pin.sha256.size()!=64||std::ranges::any_of(pin.sha256,[](char c){return !((c>='0'&&c<='9')||(c>='a'&&c<='f'));})||!pins.insert({pin.host,pin.port}).second) bad();
        if(pin.algorithm!="ssh-ed25519"&&pin.algorithm!="ecdsa-sha2-nistp256"&&pin.algorithm!="ecdsa-sha2-nistp384"&&pin.algorithm!="ecdsa-sha2-nistp521"&&pin.algorithm!="rsa-sha2-256"&&pin.algorithm!="rsa-sha2-512") bad();
        doc.knownHosts.push_back(std::move(pin));
    }
    for(const auto& n:collection(root,"snippets",1000)) {
        fields(n,{"id","name","command"}); SnippetRecord s; s.id=text(n,"id",36); s.name=text(n,"name",256);
        const auto command=field(n,"command").text(); if(command.empty()||command.size()>65536||command.find('\0')!=command.npos) bad();
        s.command=copy(command); doc.snippets.push_back(std::move(s));
    }
    const auto& settings=field(root,"settings");
    for(const auto& [key,value]:settings.members) {
        (void)value;
        if(key=="theme") { doc.settings.theme=text(settings,"theme",16); if(doc.settings.theme!="dark"&&doc.settings.theme!="light") bad(); }
        else if(key=="font") doc.settings.font=text(settings,"font",128);
        else if(key=="font_size") doc.settings.fontSize=number(settings,"font_size",8,40);
        else if(key=="right_click_paste") doc.settings.rightClickPaste=boolean(settings,"right_click_paste");
        else bad();
    }
    references(doc); return doc;
}
SecretBytes serializeDocument(const VaultDocument& doc) {
    Writer w; w.raw("{\"schema\":1,\"groups\":["); bool first=true;
    const auto separator=[&] { if(!first) w.raw(","); first=false; };
    for(const auto& n:doc.groups) { separator(); w.raw("{"); w.member("id",n.id); w.raw(",");w.member("name",n.name);w.raw(",");w.member("parent",n.parent);w.raw("}"); }
    w.raw("],\"hosts\":[");first=true;
    for(const auto& n:doc.hosts) {separator();w.raw("{");w.member("id",n.id);w.raw(",");w.member("name",n.name);w.raw(",");w.member("host",n.host);w.raw(",");w.number("port",n.port);w.raw(",");w.member("user",n.user);w.raw(",");w.member("group",n.group);w.raw(",");w.member("credential",n.credential);w.raw(",");w.member("jump",n.jump);w.raw(",\"favorite\":");w.raw(n.favorite?"true":"false");w.raw("}");}
    w.raw("],\"credentials\":[");first=true;
    for(const auto& n:doc.credentials) {separator();w.raw("{");w.member("id",n.id);w.raw(",");w.member("kind",n.kind);w.raw(",");w.member("host",n.host);w.raw(",");w.number("port",n.port);w.raw(",");w.member("user",n.user);w.raw(",");w.member("secret",view(n.secret));w.raw(",");w.member("passphrase",view(n.passphrase));w.raw(",");w.member("certificate",n.certificate);w.raw("}");}
    w.raw("],\"known_hosts\":[");first=true;
    for(const auto& n:doc.knownHosts) {separator();w.raw("{");w.member("host",n.host);w.raw(",");w.number("port",n.port);w.raw(",");w.member("algorithm",n.algorithm);w.raw(",");w.member("sha256",n.sha256);w.raw("}");}
    w.raw("],\"snippets\":[");first=true;
    for(const auto& n:doc.snippets) {separator();w.raw("{");w.member("id",n.id);w.raw(",");w.member("name",n.name);w.raw(",");w.member("command",view(n.command));w.raw("}");}
    w.raw("],\"settings\":{");w.member("theme",doc.settings.theme);w.raw(",");w.member("font",doc.settings.font);w.raw(",");w.number("font_size",doc.settings.fontSize);w.raw(",\"right_click_paste\":");w.raw(doc.settings.rightClickPaste?"true":"false");w.raw("}}");
    auto bytes=w.finish(); (void)parseDocument(bytes.bytes()); return bytes;
}
} // namespace nordssh::vault
