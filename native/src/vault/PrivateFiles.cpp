#include "PrivateFiles.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nordssh::vault {
namespace {
[[noreturn]] void fail() { throw FileError(); }
void noLinks(const QString& path) {
    auto current = QFileInfo(path).absoluteFilePath();
    for (;;) {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) fail();
#else
        struct stat st{};
        if (lstat(QFile::encodeName(current).constData(), &st) || S_ISLNK(st.st_mode)) fail();
#endif
        const auto parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) break;
        current = parent;
    }
}
#ifdef _WIN32
struct Security {
    std::vector<std::byte> user;
    PSECURITY_DESCRIPTOR descriptor{};
    Security() {
        HANDLE token{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) fail();
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        user.resize(size);
        const bool ok = size && GetTokenInformation(token, TokenUser, user.data(), size, &size);
        CloseHandle(token);
        if (!ok) fail();
        LPWSTR sidText{};
        if (!ConvertSidToStringSidW(sid(), &sidText)) fail();
        const auto sidString = QString::fromWCharArray(sidText);
        LocalFree(sidText);
        const auto sddl = QStringLiteral("O:%1D:P(A;OICI;FA;;;%1)").arg(sidString);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr)) fail();
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid; }
    void protect(qintptr descriptorFd) const {
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(static_cast<int>(descriptorFd)));
        if (handle == INVALID_HANDLE_VALUE) fail();
        // Qt opens its temporary file for data I/O, not WRITE_DAC/WRITE_OWNER.
        // Reopen the same file object (no pathname race) for security metadata.
        const auto securityHandle = ReOpenFile(handle, READ_CONTROL | WRITE_DAC | WRITE_OWNER,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
        if (securityHandle == INVALID_HANDLE_VALUE) fail();
        BOOL present{}, defaulted{}; PACL acl{};
        const bool valid = GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present && acl &&
            SetSecurityInfo(securityHandle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                sid(), nullptr, acl, nullptr) == ERROR_SUCCESS;
        CloseHandle(securityHandle);
        if (!valid) fail();
    }
    void verify(const QString& path) const {
        PSID owner{}; PACL acl{}; PSECURITY_DESCRIPTOR value{};
        if (GetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &value) != ERROR_SUCCESS) fail();
        bool valid = owner && EqualSid(owner, sid()) && acl && acl->AceCount > 0;
        for (DWORD i = 0; valid && i < acl->AceCount; ++i) {
            void* entry{};
            if (!GetAce(acl, i, &entry)) { valid = false; break; }
            const auto* header = static_cast<const ACE_HEADER*>(entry);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { valid = false; break; }
            auto* allow = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            valid = EqualSid(&allow->SidStart, sid()) != FALSE;
        }
        LocalFree(value);
        if (!valid) fail();
    }
};
void verifyPrivate(const QString& path, bool) { Security().verify(path); }
#else
void verifyPrivate(const QString& path, bool directory) {
    struct stat st{};
    if (lstat(QFile::encodeName(path).constData(), &st) || st.st_uid != geteuid() ||
        (st.st_mode & 0077) || (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) fail();
}
#endif
}
PrivateFiles::PrivateFiles(QString directory) : directory_(QFileInfo(directory).absoluteFilePath()) {
    const auto parent = QFileInfo(directory_).dir().absolutePath();
    noLinks(parent);
    if (!QFileInfo::exists(directory_)) {
#ifdef _WIN32
        Security security;
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.descriptor, FALSE};
        if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(directory_.utf16()), &attributes)) fail();
#else
        if (mkdir(QFile::encodeName(directory_).constData(), 0700)) fail();
#endif
    }
    checkDirectory();
}
void PrivateFiles::checkDirectory() const {
    noLinks(directory_);
    if (!QFileInfo(directory_).isDir()) fail();
    verifyPrivate(directory_, true);
}
QString PrivateFiles::path(const QString& name) const {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9.-]{0,119}$"));
    if (!pattern.match(name).hasMatch() || name.contains(QStringLiteral(".."))) fail();
    return QDir(directory_).filePath(name);
}
bool PrivateFiles::exists(const QString& name) const {
    checkDirectory();
    const QFileInfo info(path(name));
    if (info.isSymLink()) fail();
    return info.exists();
}
std::vector<std::byte> PrivateFiles::read(const QString& name) const {
    checkDirectory(); const auto target = path(name);
    noLinks(target); verifyPrivate(target, false);
    QFile file(target);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > static_cast<qint64>(MaxFileBytes)) fail();
    std::vector<std::byte> data(static_cast<std::size_t>(file.size()));
    if (file.read(reinterpret_cast<char*>(data.data()), static_cast<qint64>(data.size())) != static_cast<qint64>(data.size()) || !file.atEnd()) fail();
    return data;
}
void PrivateFiles::write(const QString& name, std::span<const std::byte> data, const std::function<void()>& beforeCommit) const {
    if (data.size() > MaxFileBytes) fail();
    checkDirectory(); const auto target = path(name);
    if (exists(name)) { noLinks(target); verifyPrivate(target, false); }
    QSaveFile file(target); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) fail();
#ifdef _WIN32
    Security().protect(file.handle());
#else
    if (fchmod(static_cast<int>(file.handle()), 0600)) fail();
#endif
    if (file.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size())) != static_cast<qint64>(data.size())) fail();
    if (beforeCommit) beforeCommit();
    if (!file.commit()) fail();
    verifyPrivate(target, false);
}
void PrivateFiles::forgetDeviceRecord(const QString& name) const {
    if (!name.endsWith(QStringLiteral(".device"))) fail();
    checkDirectory();
    if (!exists(name)) return;
    const auto target = path(name); noLinks(target); verifyPrivate(target, false);
    if (!QFile::remove(target)) fail();
}
} // namespace nordssh::vault
