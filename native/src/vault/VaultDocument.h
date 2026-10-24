#pragma once
#include "VaultCrypto.h"
#include <string>
#include <vector>

namespace nordssh::vault {
struct GroupRecord { std::string id, name, parent; };
struct HostRecord {
    std::string id, name, host, user, group, credential, jump;
    unsigned port = 22;
    bool favorite = false;
};
struct CredentialRecord {
    std::string id, kind, host, user, certificate;
    unsigned port = 22;
    SecretBytes secret{0}, passphrase{0};
};
struct TrustRecord { std::string host, algorithm, sha256; unsigned port = 22; };
struct SnippetRecord { std::string id, name; SecretBytes command{0}; };
struct Appearance { std::string theme = "dark", font = "monospace"; unsigned fontSize = 12; bool rightClickPaste = true; };
struct VaultDocument {
    std::vector<GroupRecord> groups;
    std::vector<HostRecord> hosts;
    std::vector<CredentialRecord> credentials;
    std::vector<TrustRecord> knownHosts;
    std::vector<SnippetRecord> snippets;
    Appearance settings;
};
class DocumentError final : public std::runtime_error {
public: DocumentError() : std::runtime_error("Vault record schema or references are invalid") {}
};
// Structural admission precedes this bounded parser. All parsed string values,
// including temporary JSON strings, use cleansed storage. No Qt JSON DOM.
[[nodiscard]] VaultDocument parseDocument(std::span<const std::byte> authenticatedPayload);
[[nodiscard]] SecretBytes serializeDocument(const VaultDocument& document);
} // namespace nordssh::vault
