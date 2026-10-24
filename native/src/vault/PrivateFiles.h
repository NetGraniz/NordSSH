#pragma once
#include "VaultFormat.h"
#include <QString>
#include <functional>
#include <stdexcept>
#include <vector>

namespace nordssh::vault {
// Shared by local transfer staging. Verify every existing ancestor; handles are
// protected before any secret/file content is written to a temporary file.
void rejectLocalLinks(const QString& path);
void protectLocalHandle(qintptr descriptorFd);
class FileError final : public std::runtime_error {
public: explicit FileError(unsigned check = 0) : std::runtime_error("Private vault file operation failed (check " + std::to_string(check) + ")") {}
};
// One private directory, created directly below an existing parent. No traversal,
// junctions or symlinks. Callers supply opaque generated basenames, not paths.
class PrivateFiles final {
public:
    explicit PrivateFiles(QString directory);
    [[nodiscard]] QString path(const QString& name) const;
    [[nodiscard]] bool exists(const QString& name) const;
    [[nodiscard]] std::vector<std::byte> read(const QString& name) const;
    void write(const QString& name, std::span<const std::byte> data,
               const std::function<void()>& beforeCommit = {}) const;
    void forgetDeviceRecord(const QString& name) const;
private:
    QString directory_;
    void checkDirectory() const;
};
} // namespace nordssh::vault
