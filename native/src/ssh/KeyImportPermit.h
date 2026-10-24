#pragma once
#include "Session.h"

namespace nordssh::ssh {
// Process-wide cap of two expensive SSH private-key decodes. Wait is bounded
// and cancellable; the third-party decode itself is not interruptible.
class KeyImportPermit final {
public:
    explicit KeyImportPermit(const OperationControl& control);
    ~KeyImportPermit();
    KeyImportPermit(const KeyImportPermit&) = delete;
    KeyImportPermit& operator=(const KeyImportPermit&) = delete;
};
} // namespace nordssh::ssh
