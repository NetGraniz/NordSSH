#include "vault/VaultDocument.h"
#include <cstring>
#include <cstdlib>
#include <iostream>

using namespace nordssh::vault;
static void require(bool value) { if(!value) throw std::runtime_error("Document contract failed"); }
template<class Action> void reject(Action action) { try { action(); } catch(const DocumentError&) { return; } throw std::runtime_error("Invalid document admitted"); }
static SecretBytes secret(std::string_view s) { SecretBytes b(s.size()); std::memcpy(b.bytes().data(),s.data(),s.size()); return b; }
int main() {
    try {
        VaultDocument doc;
        doc.groups.push_back({"10000000-0000-4000-8000-000000000001","Group",{}});
        CredentialRecord credential; credential.id="20000000-0000-4000-8000-000000000001";
        credential.kind="password"; credential.host="localhost"; credential.user="fixture"; credential.secret=secret("synthetic-secret");
        doc.credentials.push_back(std::move(credential));
        HostRecord host; host.id="30000000-0000-4000-8000-000000000001"; host.name="Fixture"; host.host="localhost";host.user="fixture";
        host.group=doc.groups[0].id;host.credential=doc.credentials[0].id;doc.hosts.push_back(std::move(host));
        doc.knownHosts.push_back({"localhost","ssh-ed25519",std::string(64,'a'),22});
        SnippetRecord snippet;snippet.id="40000000-0000-4000-8000-000000000001";snippet.name="Example";snippet.command=secret("echo test\nprintf ok\n");doc.snippets.push_back(std::move(snippet));
        const auto serialized=serializeDocument(doc); auto parsed=parseDocument(serialized.bytes());
        require(parsed.hosts.size()==1&&parsed.credentials.size()==1&&parsed.snippets.size()==1&&parsed.credentials[0].secret.bytes().size()==16);
        require(parsed.settings.theme=="dark"&&parsed.groups[0].name=="Group");
        doc.hosts[0].host="different.example";reject([&]{(void)serializeDocument(doc);});doc.hosts[0].host="localhost";
        doc.hosts[0].port=0;reject([&]{(void)serializeDocument(doc);});doc.hosts[0].port=22;
        doc.groups[0].parent=doc.groups[0].id;reject([&]{(void)serializeDocument(doc);});doc.groups[0].parent.clear();
        doc.hosts[0].jump=doc.hosts[0].id;reject([&]{(void)serializeDocument(doc);});doc.hosts[0].jump.clear();
        doc.knownHosts[0].algorithm="ssh-rsa";reject([&]{(void)serializeDocument(doc);});doc.knownHosts[0].algorithm="ssh-ed25519";
        doc.hosts[0].id=doc.groups[0].id;reject([&]{(void)serializeDocument(doc);});
        const std::string invalid="{\"schema\":1,\"hosts\":[],\"groups\":[],\"credentials\":[],\"known_hosts\":[],\"snippets\":[],\"settings\":{\"unknown\":true}}";
        reject([&]{(void)parseDocument(std::as_bytes(std::span(invalid)));});
        const std::string escape="{\"schema\":1,\"hosts\":[],\"groups\":[{\"id\":\"10000000-0000-4000-8000-000000000001\",\"name\":\"\\uD83D\\uDE80\",\"parent\":\"\"}],\"credentials\":[],\"known_hosts\":[],\"snippets\":[],\"settings\":{}}";
        auto unicode=parseDocument(std::as_bytes(std::span(escape)));require(unicode.groups[0].name=="\xf0\x9f\x9a\x80");
        for(const auto text: {"", "{}", "{\"schema\":1,\"schema\":1}", "[true]"}) reject([&]{(void)parseDocument(std::as_bytes(std::span(text,std::strlen(text))));});
        std::cout<<"Vault schema, secret roundtrip, references, bindings, unicode and negative cases passed\n";return EXIT_SUCCESS;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return EXIT_FAILURE;}
}
