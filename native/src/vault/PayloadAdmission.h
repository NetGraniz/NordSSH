#pragma once
#include <cstddef>
#include <string_view>

namespace nordssh::vault {
enum class PayloadStatus { StructureAccepted, Syntax, InvalidUtf8, ResourceLimit,
    DuplicateKey, RootSchema, UnsupportedSchema };
struct PayloadCheck {
    PayloadStatus status;
    std::size_t nodes = 0;
    bool unreviewedRecords = false;
    // Only the empty schema-1 document is a complete record schema at this stage.
    [[nodiscard]] bool emptySchemaAccepted() const noexcept {
        return status == PayloadStatus::StructureAccepted && !unreviewedRecords;
    }
};
// Call only AFTER AEAD verification. Bounded grammar/duplicate/root-shape gate,
// not product record validation. Nonempty records/settings MUST NOT be persisted
// or acted on until their module-specific schemas/references have been reviewed.
// No DOM, retained string values, I/O, logging or side effects.
[[nodiscard]] PayloadCheck inspectPayloadStructure(std::string_view authenticatedUtf8);
} // namespace nordssh::vault
