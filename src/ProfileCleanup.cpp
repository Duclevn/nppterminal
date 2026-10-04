#include "ProfileCleanup.h"

#include "Version.h"

#include <bcrypt.h>
#include <shlobj.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <utility>
#include <thread>
#include <atomic>

namespace nppterminal::profilecleanup {

namespace {

constexpr std::uint32_t kOwnerMarkerMagic = 0x4e505043u; // NPPC
constexpr std::uint16_t kOwnerMarkerVersion = 1;
constexpr std::size_t kMaxInstanceChars = 128;
constexpr std::size_t kMaxParentIdentityChars = 1024;
constexpr std::size_t kMaxCleanupEntries = 16384;
constexpr std::size_t kMaxCleanupDepth = 32;
constexpr std::size_t kMaxRecoveryCandidates = 64;
constexpr DWORD kCleanupRetryCount = 4;
constexpr DWORD kCleanupRetryDelayMs = 25;
// A contained reader can outlive the lease close briefly.  Keep the claim
// window bounded at two seconds without extending the ordinary tree-delete
// retries or the helper watchdog.
constexpr DWORD kCleanupClaimRetryCount = 80;
constexpr DWORD kCleanupDeadlineMs = 30000;
constexpr DWORD kCleanupWatchdogMs = 45000;
constexpr std::uint32_t kProcessSnapshotComplete = 0x1u;

#pragma pack(push, 1)
struct FileIdentityDisk {
    std::uint64_t volumeSerial = 0;
    std::uint64_t fileIndex = 0;
};

struct ProcessIdentityDisk {
    DWORD processId = 0;
    FILETIME creationTime{};
};

struct OwnerMarkerDisk {
    std::uint32_t magic = kOwnerMarkerMagic;
    std::uint16_t version = kOwnerMarkerVersion;
    std::uint16_t bytes = 0;
    std::uint8_t nonce[kNonceBytes]{};
    wchar_t instanceId[kMaxInstanceChars]{};
    wchar_t parentIdentity[kMaxParentIdentityChars]{};
    FileIdentityDisk rootFileId{};
    std::uint32_t phase = static_cast<std::uint32_t>(CleanupPhase::Active);
    DWORD hostProcessId = 0;
    FILETIME hostCreationTime{};
    std::uint32_t browserProcessCount = 0;
    std::uint32_t flags = 0;
    ProcessIdentityDisk browserProcesses[kMaxTrackedProcesses]{};
};
#pragma pack(pop)

static_assert(sizeof(OwnerMarkerDisk) < 64u * 1024u,
    "owner marker must remain a small bounded file");

struct MarkerSnapshot {
    std::wstring instanceId;
    std::wstring parentIdentity;
    std::array<std::uint8_t, kNonceBytes> nonce{};
    FileIdentity rootFileId{};
    ProcessIdentity hostProcess{};
    std::vector<ProcessIdentity> browserProcesses;
    bool processSnapshotComplete = false;
    CleanupPhase phase = CleanupPhase::Active;
};

struct TreeEntry {
    std::filesystem::path path;
    bool directory = false;
};

enum class ProcessStatus {
    Inactive,
    Active,
    Unknown,
};

struct CleanupDeadline {
    ULONGLONG endTick = 0;

    explicit CleanupDeadline(DWORD durationMs)
        : endTick(::GetTickCount64() + durationMs)
    {
    }

    bool expired() const
    {
        return ::GetTickCount64() >= endTick;
    }
};

std::wstring win32Error(DWORD code = ERROR_SUCCESS)
{
    if (code == ERROR_SUCCESS) code = ::GetLastError();
    wchar_t buffer[512] = {};
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    if (length == 0) return L"Windows error " + std::to_wstring(code);
    std::wstring message(buffer, length);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

bool validHandle(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

void closeHandle(HANDLE& handle)
{
    if (validHandle(handle)) {
        ::CloseHandle(handle);
    }
    handle = nullptr;
}

std::wstring trimPath(std::wstring value)
{
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

bool canonicalPath(const std::wstring& input, std::wstring& output)
{
    if (input.empty()) return false;
    std::vector<wchar_t> buffer(4096);
    for (;;) {
        const DWORD length = ::GetFullPathNameW(input.c_str(),
            static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length == 0) return false;
        if (length < buffer.size()) {
            output = trimPath(std::wstring(buffer.data(), length));
            return !output.empty();
        }
        if (length > 32768 || length + 1 <= buffer.size()) return false;
        buffer.resize(length + 1);
    }
}

bool samePath(const std::wstring& left, const std::wstring& right)
{
    std::wstring canonicalLeft;
    std::wstring canonicalRight;
    return canonicalPath(left, canonicalLeft) && canonicalPath(right, canonicalRight) &&
        !_wcsicmp(canonicalLeft.c_str(), canonicalRight.c_str());
}

bool isNotReparsePoint(const std::wstring& path, bool directory)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    if (directory && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
    return true;
}

bool isPathTreeNotReparse(const std::wstring& path)
{
    std::filesystem::path current(path);
    for (;;) {
        if (!isNotReparsePoint(current.wstring(), true)) return false;
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || samePath(parent.wstring(), current.wstring())) break;
        current = parent;
    }
    return true;
}

bool getFileIdentity(const std::wstring& path, FileIdentity& identity, bool directory)
{
    identity = {};
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
        (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        flags, nullptr);
    if (!validHandle(handle)) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL result = ::GetFileInformationByHandle(handle, &info);
    ::CloseHandle(handle);
    if (!result) return false;
    identity.volumeSerial = info.dwVolumeSerialNumber;
    identity.fileIndex = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
        info.nFileIndexLow;
    return true;
}

bool sameFileIdentity(const FileIdentity& left, const FileIdentity& right)
{
    return left.volumeSerial != 0 && left.volumeSerial == right.volumeSerial &&
        left.fileIndex != 0 && left.fileIndex == right.fileIndex;
}

bool hasFileIdentity(const FileIdentity& identity)
{
    return identity.volumeSerial != 0 && identity.fileIndex != 0;
}

bool hasProcessCreationTime(const FILETIME& creationTime)
{
    return creationTime.dwHighDateTime != 0 || creationTime.dwLowDateTime != 0;
}

bool hasNonce(const std::array<std::uint8_t, kNonceBytes>& nonce)
{
    return std::any_of(nonce.begin(), nonce.end(), [](std::uint8_t value) {
        return value != 0;
    });
}

bool ensureParentDirectory(const std::wstring& parent, std::wstring& error)
{
    DWORD attributes = ::GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (::SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr) != ERROR_SUCCESS) {
            attributes = ::GetFileAttributesW(parent.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                error = L"Unable to create the WebView profile parent: " + win32Error();
                return false;
            }
        }
    }
    if ((attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) ||
        !isPathTreeNotReparse(parent)) {
        error = L"The WebView profile parent is not a normal directory.";
        return false;
    }
    return true;
}

bool validInstanceId(const std::wstring& value)
{
    constexpr std::size_t kGuidChars = 36;
    if (value.size() != (std::size(kInstancePrefix) - 1) + kGuidChars ||
        value.rfind(kInstancePrefix, 0) != 0) return false;
    if (value.find_first_of(L"\\/:*?\"<>|\r\n\t") != std::wstring::npos) return false;
    const std::wstring guidText = L"{" + value.substr(std::size(kInstancePrefix) - 1) + L"}";
    GUID guid{};
    return SUCCEEDED(::CLSIDFromString(guidText.c_str(), &guid));
}

bool validTombstoneName(const std::wstring& value)
{
    constexpr wchar_t prefix[] = L"cleanup-v1-";
    constexpr std::size_t kGuidChars = 36;
    if (value.size() != (std::size(prefix) - 1) + kGuidChars ||
        value.rfind(prefix, 0) != 0) return false;
    return validInstanceId(std::wstring(kInstancePrefix) +
        value.substr(std::size(prefix) - 1));
}

std::wstring nonceHex(const std::array<std::uint8_t, kNonceBytes>& nonce)
{
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(kNonceBytes * 2);
    for (const std::uint8_t byte : nonce) {
        result.push_back(digits[(byte >> 4) & 0xf]);
        result.push_back(digits[byte & 0xf]);
    }
    return result;
}

int hexValue(wchar_t value)
{
    if (value >= L'0' && value <= L'9') return value - L'0';
    if (value >= L'a' && value <= L'f') return value - L'a' + 10;
    if (value >= L'A' && value <= L'F') return value - L'A' + 10;
    return -1;
}

bool parseNonce(const std::wstring& value, std::array<std::uint8_t, kNonceBytes>& nonce)
{
    if (value.size() != kNonceBytes * 2) return false;
    for (std::size_t index = 0; index < kNonceBytes; ++index) {
        const int high = hexValue(value[index * 2]);
        const int low = hexValue(value[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        nonce[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool copyFixed(wchar_t* destination, std::size_t capacity, const std::wstring& value)
{
    if (value.empty() || value.size() >= capacity) return false;
    std::wmemset(destination, L'\0', capacity);
    std::wmemcpy(destination, value.data(), value.size());
    return true;
}

bool readFixed(const wchar_t* source, std::size_t capacity, std::wstring& value)
{
    const std::size_t length = wcsnlen_s(source, capacity);
    if (length == 0 || length >= capacity) return false;
    value.assign(source, length);
    return true;
}

bool writeAll(HANDLE handle, const void* bytes, DWORD count)
{
    const auto* source = static_cast<const std::uint8_t*>(bytes);
    DWORD total = 0;
    while (total < count) {
        DWORD written = 0;
        if (!::WriteFile(handle, source + total, count - total, &written, nullptr) ||
            written == 0) return false;
        total += written;
    }
    return true;
}

bool readAll(HANDLE handle, void* bytes, DWORD count)
{
    auto* destination = static_cast<std::uint8_t*>(bytes);
    DWORD total = 0;
    while (total < count) {
        DWORD received = 0;
        if (!::ReadFile(handle, destination + total, count - total, &received, nullptr) ||
            received == 0) return false;
        total += received;
    }
    return true;
}

bool makeMarker(const OwnedProfile& profile, OwnerMarkerDisk& marker, std::wstring& error)
{
    marker = {};
    marker.magic = kOwnerMarkerMagic;
    marker.version = kOwnerMarkerVersion;
    marker.bytes = sizeof(marker);
    std::copy(profile.nonce.begin(), profile.nonce.end(), std::begin(marker.nonce));
    if (!copyFixed(marker.instanceId, std::size(marker.instanceId), profile.instanceId) ||
        !copyFixed(marker.parentIdentity, std::size(marker.parentIdentity), profile.parentIdentity)) {
        error = L"The WebView profile identity is longer than the marker schema allows.";
        return false;
    }
    marker.rootFileId.volumeSerial = profile.rootFileId.volumeSerial;
    marker.rootFileId.fileIndex = profile.rootFileId.fileIndex;
    marker.phase = static_cast<std::uint32_t>(profile.phase);
    marker.hostProcessId = profile.hostProcess.processId;
    marker.hostCreationTime = profile.hostProcess.creationTime;
    if (profile.browserProcesses.size() > kMaxTrackedProcesses) {
        error = L"The WebView process family is larger than the bounded marker schema allows.";
        return false;
    }
    marker.browserProcessCount = static_cast<std::uint32_t>(profile.browserProcesses.size());
    marker.flags = profile.processSnapshotComplete ? kProcessSnapshotComplete : 0;
    for (std::size_t index = 0; index < profile.browserProcesses.size(); ++index) {
        marker.browserProcesses[index].processId = profile.browserProcesses[index].processId;
        marker.browserProcesses[index].creationTime = profile.browserProcesses[index].creationTime;
    }
    return true;
}

bool writeMarker(const OwnedProfile& profile, bool createNew, std::wstring& error)
{
    OwnerMarkerDisk marker{};
    if (!makeMarker(profile, marker, error)) return false;
    const std::wstring path = profile.rootPath + L"\\" + kOwnerMarkerName;
    const DWORD disposition = createNew ? CREATE_NEW : OPEN_EXISTING;
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (!validHandle(handle)) {
        error = L"Unable to open the WebView owner marker: " + win32Error();
        return false;
    }
    LARGE_INTEGER offset{};
    const bool positioned = ::SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN) != FALSE;
    const bool written = positioned && writeAll(handle, &marker, sizeof(marker));
    const bool truncated = written && ::SetEndOfFile(handle) != FALSE;
    const bool flushed = truncated && ::FlushFileBuffers(handle) != FALSE;
    ::CloseHandle(handle);
    if (!flushed) {
        error = L"Unable to persist the WebView owner marker: " + win32Error();
        return false;
    }
    return true;
}

bool readMarkerFile(const std::wstring& path, MarkerSnapshot& snapshot, std::wstring& error)
{
    snapshot = {};
    if (!isNotReparsePoint(path, false)) {
        error = L"The WebView owner marker is a reparse point or is unavailable.";
        return false;
    }
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (!validHandle(handle)) {
        error = L"The WebView owner marker could not be opened: " + win32Error();
        return false;
    }
    LARGE_INTEGER size{};
    const bool sized = ::GetFileSizeEx(handle, &size) != FALSE;
    OwnerMarkerDisk marker{};
    const bool read = sized && size.QuadPart == sizeof(marker) && readAll(handle, &marker, sizeof(marker));
    ::CloseHandle(handle);
    if (!read || marker.magic != kOwnerMarkerMagic || marker.version != kOwnerMarkerVersion ||
        marker.bytes != sizeof(marker) || marker.browserProcessCount > kMaxTrackedProcesses ||
        marker.hostProcessId == 0 || !hasProcessCreationTime(marker.hostCreationTime) ||
        marker.rootFileId.volumeSerial == 0 || marker.rootFileId.fileIndex == 0 ||
        !readFixed(marker.instanceId, std::size(marker.instanceId), snapshot.instanceId) ||
        !readFixed(marker.parentIdentity, std::size(marker.parentIdentity), snapshot.parentIdentity)) {
        error = L"The WebView owner marker is malformed or has an unsupported schema.";
        return false;
    }
    std::copy(std::begin(marker.nonce), std::end(marker.nonce), snapshot.nonce.begin());
    if (!hasNonce(snapshot.nonce)) {
        error = L"The WebView owner marker contains an invalid nonce.";
        return false;
    }
    snapshot.rootFileId.volumeSerial = marker.rootFileId.volumeSerial;
    snapshot.rootFileId.fileIndex = marker.rootFileId.fileIndex;
    snapshot.phase = static_cast<CleanupPhase>(marker.phase);
    if (snapshot.phase != CleanupPhase::Active && snapshot.phase != CleanupPhase::Closing &&
        snapshot.phase != CleanupPhase::Released) {
        error = L"The WebView owner marker has an invalid cleanup phase.";
        return false;
    }
    snapshot.hostProcess.processId = marker.hostProcessId;
    snapshot.hostProcess.creationTime = marker.hostCreationTime;
    snapshot.processSnapshotComplete = (marker.flags & kProcessSnapshotComplete) != 0;
    snapshot.browserProcesses.reserve(marker.browserProcessCount);
    for (std::size_t index = 0; index < marker.browserProcessCount; ++index) {
        ProcessIdentity identity;
        identity.processId = marker.browserProcesses[index].processId;
        identity.creationTime = marker.browserProcesses[index].creationTime;
        if (identity.processId == 0 || !hasProcessCreationTime(identity.creationTime)) {
            error = L"The WebView owner marker contains an invalid process identity.";
            return false;
        }
        snapshot.browserProcesses.push_back(identity);
    }
    return true;
}

bool readMarker(const std::wstring& rootPath, MarkerSnapshot& snapshot, std::wstring& error)
{
    return readMarkerFile(rootPath + L"\\" + kOwnerMarkerName, snapshot, error);
}

bool validateRoot(const std::wstring& parentPath, const std::wstring& instanceId,
    const std::wstring& rootPath, const MarkerSnapshot& marker,
    const std::array<std::uint8_t, kNonceBytes>& nonce, std::wstring& error)
{
    std::wstring canonicalParent;
    std::wstring canonicalRoot;
    if (!canonicalPath(parentPath, canonicalParent) || !canonicalPath(rootPath, canonicalRoot) ||
        !samePath(std::filesystem::path(canonicalRoot).parent_path().wstring(), canonicalParent) ||
        !validInstanceId(instanceId) || !samePath(canonicalRoot, rootPath) ||
        marker.instanceId != instanceId || !samePath(marker.parentIdentity, canonicalParent) ||
        !std::equal(nonce.begin(), nonce.end(), marker.nonce.begin()) ||
        !isPathTreeNotReparse(canonicalParent) || !isPathTreeNotReparse(canonicalRoot)) {
        error = L"The WebView owner marker does not identify the exact owned directory.";
        return false;
    }
    FileIdentity rootId;
    if (!getFileIdentity(canonicalRoot, rootId, true) || !sameFileIdentity(rootId, marker.rootFileId)) {
        error = L"The WebView owned-directory identity no longer matches its marker.";
        return false;
    }
    return true;
}

ProcessStatus processIdentityStatus(const ProcessIdentity& identity)
{
    if (identity.processId == 0) return ProcessStatus::Unknown;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
        identity.processId);
    if (!process) {
        const DWORD error = ::GetLastError();
        return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND ||
            error == ERROR_FILE_NOT_FOUND ?
            ProcessStatus::Inactive : ProcessStatus::Unknown;
    }
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    const bool timesRead = ::GetProcessTimes(process, &creation, &exitTime, &kernel, &user) != FALSE;
    const bool identityMatches = timesRead &&
        creation.dwLowDateTime == identity.creationTime.dwLowDateTime &&
        creation.dwHighDateTime == identity.creationTime.dwHighDateTime;
    const DWORD waitResult = identityMatches ? ::WaitForSingleObject(process, 0) : WAIT_OBJECT_0;
    ::CloseHandle(process);
    if (!timesRead) return ProcessStatus::Unknown;
    if (!identityMatches || waitResult == WAIT_OBJECT_0) return ProcessStatus::Inactive;
    if (waitResult == WAIT_TIMEOUT) return ProcessStatus::Active;
    return ProcessStatus::Unknown;
}

bool getLocalAppData(std::wstring& path)
{
    path.clear();
    PWSTR value = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value)) ||
        !value) {
        if (value) ::CoTaskMemFree(value);
        return false;
    }
    path.assign(value);
    ::CoTaskMemFree(value);
    return !path.empty();
}

bool makeInstanceId(std::wstring& instanceId, std::array<std::uint8_t, kNonceBytes>& nonce)
{
    std::array<std::uint8_t, sizeof(GUID) * 2> randomBytes{};
    if (BCryptGenRandom(nullptr, randomBytes.data(),
        static_cast<ULONG>(randomBytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return false;
    }
    GUID id{};
    std::memcpy(&id, randomBytes.data(), sizeof(id));
    // RFC 4122 version 4 / variant bits make the directory identity a normal
    // GUID while the bytes themselves come from the system CSPRNG.
    id.Data3 = static_cast<decltype(id.Data3)>((id.Data3 & 0x0fffu) | 0x4000u);
    id.Data4[0] = static_cast<unsigned char>((id.Data4[0] & 0x3fu) | 0x80u);
    std::memcpy(nonce.data(), randomBytes.data() + sizeof(id), nonce.size());
    if (!hasNonce(nonce)) return false;
    wchar_t text[64] = {};
    if (::StringFromGUID2(id, text, static_cast<int>(std::size(text))) <= 0) return false;
    std::wstring guidText(text);
    if (guidText.size() < 2 || guidText.front() != L'{' || guidText.back() != L'}') return false;
    guidText = guidText.substr(1, guidText.size() - 2);
    instanceId = std::wstring(kInstancePrefix) + guidText;
    return validInstanceId(instanceId);
}

bool makeTombstoneName(std::wstring& name)
{
    std::wstring id;
    std::array<std::uint8_t, kNonceBytes> ignored{};
    if (!makeInstanceId(id, ignored)) return false;
    name = L"cleanup-v1-" + id.substr(std::size(kInstancePrefix) - 1);
    return true;
}

bool helperVersionMatches(const std::wstring& path, std::wstring& error)
{
    DWORD ignored = 0;
    const DWORD bytes = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (bytes == 0) {
        error = L"The cleanup helper has no readable version resource: " + win32Error();
        return false;
    }
    std::vector<std::uint8_t> storage(bytes);
    if (!::GetFileVersionInfoW(path.c_str(), 0, bytes, storage.data())) {
        error = L"The cleanup helper version resource could not be read: " + win32Error();
        return false;
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoBytes = 0;
    if (!::VerQueryValueW(storage.data(), L"\\", reinterpret_cast<void**>(&info), &infoBytes) ||
        !info || infoBytes < sizeof(VS_FIXEDFILEINFO)) {
        error = L"The cleanup helper version resource is malformed.";
        return false;
    }
    const unsigned int major = HIWORD(info->dwFileVersionMS);
    const unsigned int minor = LOWORD(info->dwFileVersionMS);
    const unsigned int patch = HIWORD(info->dwFileVersionLS);
    if (major != NPPTERMINAL_VERSION_MAJOR || minor != NPPTERMINAL_VERSION_MINOR ||
        patch != NPPTERMINAL_VERSION_PATCH) {
        error = L"The cleanup helper version does not match NppTerminal.";
        return false;
    }
    return true;
}

bool acquireLeasePath(const std::wstring& path, HANDLE& lease, std::wstring& error,
    bool allowDeleteShare = false)
{
    if (!isNotReparsePoint(path, false)) {
        error = L"The WebView profile lease is a reparse point or is unavailable.";
        return false;
    }
    lease = ::CreateFileW(path.c_str(),
        GENERIC_READ | GENERIC_WRITE, allowDeleteShare ? FILE_SHARE_DELETE : 0,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (!validHandle(lease)) {
        const DWORD openError = ::GetLastError();
        lease = nullptr;
        if (openError == ERROR_SHARING_VIOLATION) {
            error = L"The WebView profile lease is still held.";
        } else {
            error = L"The WebView profile lease could not be acquired: " + win32Error(openError);
        }
        return false;
    }
    return true;
}

bool acquireLeaseFile(const std::wstring& rootPath, HANDLE& lease, std::wstring& error,
    bool allowDeleteShare = false)
{
    return acquireLeasePath(rootPath + L"\\" + kLeaseName, lease, error, allowDeleteShare);
}

class CleanupReservation final {
public:
    CleanupReservation() = default;
    CleanupReservation(const CleanupReservation&) = delete;
    CleanupReservation& operator=(const CleanupReservation&) = delete;

    ~CleanupReservation()
    {
        if (validHandle(handle_)) {
            (void)::ReleaseMutex(handle_);
            closeHandle(handle_);
        }
    }

    bool acquire(const std::wstring& instanceId,
        const std::array<std::uint8_t, kNonceBytes>& nonce, std::wstring& error)
    {
        const std::wstring name = L"Local\\NppTerminal.ProfileCleanup.v1." +
            instanceId + L"." + nonceHex(nonce);
        ::SetLastError(ERROR_SUCCESS);
        handle_ = ::CreateMutexW(nullptr, TRUE, name.c_str());
        if (!validHandle(handle_)) {
            error = L"Unable to reserve the WebView cleanup identity: " + win32Error();
            return false;
        }
        if (::GetLastError() == ERROR_ALREADY_EXISTS) {
            closeHandle(handle_);
            error = L"The WebView cleanup identity is already reserved.";
            return false;
        }
        return true;
    }

private:
    HANDLE handle_ = nullptr;
};

bool isSafeChild(const std::filesystem::path& root, const std::filesystem::path& child)
{
    std::wstring canonicalRoot;
    std::wstring canonicalChild;
    if (!canonicalPath(root.wstring(), canonicalRoot) ||
        !canonicalPath(child.wstring(), canonicalChild)) return false;
    const std::wstring prefix = trimPath(canonicalRoot) + L"\\";
    return canonicalChild.size() > prefix.size() &&
        !_wcsnicmp(canonicalChild.c_str(), prefix.c_str(), prefix.size());
}

bool collectTree(const std::filesystem::path& root, const std::filesystem::path& current,
    std::size_t depth, std::vector<TreeEntry>& entries, CleanupDeadline& deadline,
    std::wstring& error)
{
    if (deadline.expired()) {
        error = L"The WebView cleanup deadline expired while scanning.";
        return false;
    }
    if (depth > kMaxCleanupDepth ||
        ((!isSafeChild(root, current)) && !samePath(root.wstring(), current.wstring())) ||
        !isNotReparsePoint(current.wstring(), true)) {
        error = L"The WebView cleanup tree contains an unsafe path or reparse point.";
        return false;
    }
    const std::wstring pattern = current.wstring() + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        error = L"The WebView cleanup tree could not be scanned: " + win32Error();
        return false;
    }
    bool success = true;
    do {
        if (deadline.expired()) {
            error = L"The WebView cleanup deadline expired while scanning.";
            success = false;
            break;
        }
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..") continue;
        if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos) {
            error = L"The WebView cleanup tree contains an invalid child name.";
            success = false;
            break;
        }
        const std::filesystem::path child = current / name;
        if (entries.size() >= kMaxCleanupEntries || !isSafeChild(root, child) ||
            !isNotReparsePoint(child.wstring(), (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
            error = L"The WebView cleanup tree exceeds the safety bounds or contains a reparse point.";
            success = false;
            break;
        }
        const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entries.push_back(TreeEntry{child, directory});
        if (directory && !collectTree(root, child, depth + 1, entries, deadline, error)) {
            success = false;
            break;
        }
    } while (::FindNextFileW(find, &data));
    const DWORD findError = ::GetLastError();
    ::FindClose(find);
    if (success && findError != ERROR_NO_MORE_FILES) {
        error = L"The WebView cleanup tree scan did not complete: " + win32Error(findError);
        success = false;
    }
    return success;
}

void restoreTombstone(const std::wstring& tombstone, const std::wstring& root)
{
    if (::GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
        (void)::MoveFileExW(tombstone.c_str(), root.c_str(), MOVEFILE_WRITE_THROUGH);
    }
}

std::wstring sidecarOwnerPath(const std::wstring& tombstone)
{
    return tombstone + L".owner.bin";
}

std::wstring sidecarLeasePath(const std::wstring& tombstone)
{
    return tombstone + L".lease.bin";
}

bool moveMetadataToSidecars(const std::wstring& tombstone, CleanupDeadline& deadline,
    std::wstring& error)
{
    if (deadline.expired()) {
        error = L"The WebView cleanup deadline expired before metadata reservation.";
        return false;
    }
    const std::wstring owner = tombstone + L"\\" + kOwnerMarkerName;
    const std::wstring lease = tombstone + L"\\" + kLeaseName;
    const std::wstring ownerSidecar = sidecarOwnerPath(tombstone);
    const std::wstring leaseSidecar = sidecarLeasePath(tombstone);
    const bool ownerInside = isNotReparsePoint(owner, false);
    const bool leaseInside = isNotReparsePoint(lease, false);
    const bool ownerOutside = isNotReparsePoint(ownerSidecar, false);
    const bool leaseOutside = isNotReparsePoint(leaseSidecar, false);

    // A helper can be terminated between the two MoveFileEx calls. Accept and
    // finish any split state, but never proceed without both pieces of proof.
    if (ownerOutside && leaseOutside && !ownerInside && !leaseInside) return true;
    if (ownerInside && leaseInside && !ownerOutside && !leaseOutside) {
        if (!::MoveFileExW(owner.c_str(), ownerSidecar.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = L"Unable to reserve the WebView cleanup owner marker.";
            return false;
        }
        if (deadline.expired()) {
            error = L"The WebView cleanup deadline expired while reserving metadata.";
            return false;
        }
        if (!::MoveFileExW(lease.c_str(), leaseSidecar.c_str(), MOVEFILE_WRITE_THROUGH)) {
            // Keep the tombstone discoverable if the rollback itself races or
            // fails. The marker is still present in one of the two locations.
            (void)::MoveFileExW(ownerSidecar.c_str(), owner.c_str(), MOVEFILE_WRITE_THROUGH);
            error = L"Unable to reserve the WebView cleanup lease metadata.";
            return false;
        }
        return true;
    }
    if (ownerOutside && leaseInside && !leaseOutside) {
        if (deadline.expired()) return false;
        if (::MoveFileExW(lease.c_str(), leaseSidecar.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        error = L"Unable to reserve the WebView cleanup lease metadata.";
        return false;
    }
    if (ownerInside && leaseOutside && !ownerOutside) {
        if (deadline.expired()) return false;
        if (::MoveFileExW(owner.c_str(), ownerSidecar.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        error = L"Unable to reserve the WebView cleanup owner marker.";
        return false;
    }
    error = L"Unable to reserve the WebView cleanup metadata.";
    return false;
}

bool restoreMetadataFromSidecars(const std::wstring& tombstone, CleanupDeadline& deadline)
{
    if (deadline.expired()) return false;
    const std::wstring owner = tombstone + L"\\" + kOwnerMarkerName;
    const std::wstring lease = tombstone + L"\\" + kLeaseName;
    const std::wstring ownerSidecar = sidecarOwnerPath(tombstone);
    const std::wstring leaseSidecar = sidecarLeasePath(tombstone);
    const bool ownerInside = isNotReparsePoint(owner, false);
    const bool leaseInside = isNotReparsePoint(lease, false);
    const bool ownerOutside = isNotReparsePoint(ownerSidecar, false);
    const bool leaseOutside = isNotReparsePoint(leaseSidecar, false);
    if ((ownerInside && ownerOutside) || (leaseInside && leaseOutside)) return false;
    if (!leaseInside && leaseOutside) {
        if (deadline.expired()) return false;
        if (!::MoveFileExW(leaseSidecar.c_str(), lease.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return false;
        }
    }
    if (!ownerInside && ownerOutside) {
        if (deadline.expired()) return false;
        if (!::MoveFileExW(ownerSidecar.c_str(), owner.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return false;
        }
    }
    return isNotReparsePoint(owner, false) && isNotReparsePoint(lease, false);
}

bool deleteOneSidecar(const std::wstring& path, CleanupDeadline& deadline)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    for (DWORD attempt = 0; attempt != kCleanupRetryCount; ++attempt) {
        if (deadline.expired()) return false;
        (void)::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (::DeleteFileW(path.c_str())) return true;
        if (!deadline.expired()) ::Sleep(kCleanupRetryDelayMs);
    }
    return false;
}

bool deleteTree(const std::wstring& tombstone, CleanupDeadline& deadline, std::wstring& error)
{
    std::vector<TreeEntry> entries;
    const std::filesystem::path root(tombstone);
    if (!collectTree(root, root, 0, entries, deadline, error)) return false;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (deadline.expired()) {
            error = L"The WebView cleanup deadline expired while deleting.";
            return false;
        }
        bool removed = false;
        for (DWORD attempt = 0; attempt != kCleanupRetryCount; ++attempt) {
            if (deadline.expired()) break;
            if (!it->directory) {
                (void)::SetFileAttributesW(it->path.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
            }
            const BOOL result = it->directory ?
                ::RemoveDirectoryW(it->path.wstring().c_str()) :
                ::DeleteFileW(it->path.wstring().c_str());
            if (result) {
                removed = true;
                break;
            }
            if (!deadline.expired()) ::Sleep(kCleanupRetryDelayMs);
        }
        if (!removed) {
            error = L"The WebView cleanup tree could not be removed: " + win32Error();
            return false;
        }
    }
    for (DWORD attempt = 0; attempt != kCleanupRetryCount; ++attempt) {
        if (deadline.expired()) break;
        if (::RemoveDirectoryW(tombstone.c_str())) return true;
        if (!deadline.expired()) ::Sleep(kCleanupRetryDelayMs);
    }
    error = L"The WebView cleanup directory could not be removed: " + win32Error();
    return false;
}

int cleanupProfile(const std::wstring& parentPath, const std::wstring& instanceId,
    const std::array<std::uint8_t, kNonceBytes>& nonce, std::wstring& error,
    CleanupDeadline* inheritedDeadline = nullptr)
{
    CleanupDeadline localDeadline(kCleanupDeadlineMs);
    CleanupDeadline& deadline = inheritedDeadline ? *inheritedDeadline : localDeadline;
    std::wstring parent;
    if (!canonicalPath(parentPath, parent) || !validInstanceId(instanceId) ||
        !isPathTreeNotReparse(parent)) {
        error = L"The WebView cleanup parent is invalid.";
        return 2;
    }
    const std::wstring root = parent + L"\\" + instanceId;
    MarkerSnapshot marker;
    if (!readMarker(root, marker, error)) return 2;
    if (marker.phase != CleanupPhase::Released ||
        !validateRoot(parent, instanceId, root, marker, nonce, error)) return 2;
    CleanupReservation reservation;
    if (!reservation.acquire(instanceId, nonce, error)) return 2;
    for (const ProcessIdentity& process : marker.browserProcesses) {
        if (deadline.expired()) {
            error = L"The WebView cleanup deadline expired while checking processes.";
            return 2;
        }
        if (processIdentityStatus(process) != ProcessStatus::Inactive) {
            error = L"The WebView process family is still active.";
            return 2;
        }
    }

    HANDLE lease = nullptr;
    if (!acquireLeaseFile(root, lease, error, true)) return 2;
    MarkerSnapshot lockedMarker;
    if (!readMarker(root, lockedMarker, error) || lockedMarker.phase != CleanupPhase::Released ||
        !validateRoot(parent, instanceId, root, lockedMarker, nonce, error)) {
        closeHandle(lease);
        return 2;
    }

    std::wstring tombstoneName;
    if (!makeTombstoneName(tombstoneName)) {
        closeHandle(lease);
        error = L"Unable to generate the WebView cleanup tombstone name.";
        return 2;
    }
    const std::wstring tombstone = parent + L"\\" + tombstoneName;
    // Windows rejects renaming a directory while a handle to a contained file
    // is open, even when that handle allows FILE_SHARE_DELETE.  The marker,
    // nonce, and root identity have been revalidated while the lease was held;
    // the per-profile reservation keeps another cleanup helper from acquiring
    // the lease during this close-and-claim handoff.  The host never reopens a
    // Released profile, and a failed claim leaves the marked root available
    // for retry.
    closeHandle(lease);
    bool claimed = false;
    DWORD claimError = ERROR_SUCCESS;
    for (DWORD attempt = 0; attempt != kCleanupClaimRetryCount; ++attempt) {
        if (deadline.expired()) break;
        if (::MoveFileExW(root.c_str(), tombstone.c_str(), MOVEFILE_WRITE_THROUGH)) {
            claimed = true;
            break;
        }
        claimError = ::GetLastError();
        // Another validator can still be closing a contained handle after our
        // lease closes. Retry only transient sharing/access failures; the
        // claimed directory must still pass the identity checks below.
        if (claimError != ERROR_SHARING_VIOLATION && claimError != ERROR_ACCESS_DENIED) break;
        if (attempt + 1 < kCleanupClaimRetryCount && !deadline.expired()) {
            ::Sleep(kCleanupRetryDelayMs);
        }
    }
    if (!claimed) {
        error = deadline.expired() ? L"The WebView cleanup deadline expired before the claim." :
            L"Unable to reserve the WebView cleanup directory: " + win32Error(claimError);
        return 2;
    }

    FileIdentity tombstoneId;
    MarkerSnapshot tombstoneMarker;
    if (!isNotReparsePoint(tombstone, true) || !getFileIdentity(tombstone, tombstoneId, true) ||
        !sameFileIdentity(tombstoneId, lockedMarker.rootFileId) ||
        !readMarker(tombstone, tombstoneMarker, error) ||
        tombstoneMarker.instanceId != instanceId ||
        !std::equal(nonce.begin(), nonce.end(), tombstoneMarker.nonce.begin()) ||
        !samePath(tombstoneMarker.parentIdentity, parent)) {
        restoreTombstone(tombstone, root);
        error = L"The WebView cleanup tombstone failed its identity check.";
        return 2;
    }
    if (!moveMetadataToSidecars(tombstone, deadline, error)) {
        // Keep the marked tombstone in place.  A helper can be interrupted
        // during the two metadata moves; moving it back before both proofs are
        // visible could restore an unmarked or partially-owned root.
        return 2;
    }
    if (!deleteTree(tombstone, deadline, error)) {
        if (restoreMetadataFromSidecars(tombstone, deadline)) {
            restoreTombstone(tombstone, root);
        }
        return 2;
    }
    // Remove the lease before owner.bin.  If the final metadata removal
    // fails, the owner marker remains as a recoverable proof of ownership.
    if (!deleteOneSidecar(sidecarLeasePath(tombstone), deadline) ||
        !deleteOneSidecar(sidecarOwnerPath(tombstone), deadline)) {
        error = L"The WebView cleanup metadata could not be removed before the deadline.";
        return 2;
    }
    return 0;
}

int recoverOneProfile(const std::wstring& parent, const std::wstring& instanceId,
    CleanupDeadline& deadline)
{
    if (!validInstanceId(instanceId)) return 0;
    const std::wstring root = parent + L"\\" + instanceId;
    MarkerSnapshot marker;
    std::wstring ignored;
    if (!readMarker(root, marker, ignored) ||
        !validateRoot(parent, instanceId, root, marker, marker.nonce, ignored)) return 0;
    if (marker.phase == CleanupPhase::Released) {
        return cleanupProfile(parent, instanceId, marker.nonce, ignored, &deadline);
    }
    // A host-death PID snapshot cannot prove that WebView2 released all of
    // its browser, renderer, GPU, and profile resources.  Only the live
    // BrowserProcessExited handler may write Released; leave Closing folders
    // untouched for explicit/manual recovery.
    return 0;
}

bool validateReleasedMarkerForParent(const std::wstring& parent,
    const MarkerSnapshot& marker, std::wstring& error)
{
    std::wstring canonicalParent;
    if (!canonicalPath(parent, canonicalParent) || marker.phase != CleanupPhase::Released ||
        !validInstanceId(marker.instanceId) || !hasFileIdentity(marker.rootFileId) ||
        !samePath(marker.parentIdentity, canonicalParent) ||
        !isPathTreeNotReparse(canonicalParent)) {
        error = L"The WebView cleanup marker does not identify this parent.";
        return false;
    }
    return true;
}

bool validateTombstoneMarker(const std::wstring& parent, const std::wstring& tombstone,
    const MarkerSnapshot& marker, std::wstring& error)
{
    std::wstring canonicalParent;
    std::wstring canonicalTombstone;
    if (!canonicalPath(parent, canonicalParent) || !canonicalPath(tombstone, canonicalTombstone) ||
        !validateReleasedMarkerForParent(canonicalParent, marker, error) ||
        !validTombstoneName(std::filesystem::path(canonicalTombstone).filename().wstring()) ||
        !samePath(std::filesystem::path(canonicalTombstone).parent_path().wstring(), canonicalParent) ||
        !isNotReparsePoint(canonicalTombstone, true)) {
        error = L"The WebView cleanup tombstone marker is invalid.";
        return false;
    }
    FileIdentity tombstoneId;
    if (!getFileIdentity(canonicalTombstone, tombstoneId, true) ||
        !sameFileIdentity(tombstoneId, marker.rootFileId)) {
        error = L"The WebView cleanup tombstone identity does not match its marker.";
        return false;
    }
    return true;
}

int recoverTombstone(const std::wstring& parent, const std::wstring& tombstone,
    const std::wstring& ownerSidecar, CleanupDeadline& deadline)
{
    if (deadline.expired()) return 2;
    std::wstring ignored;
    std::wstring canonicalParent;
    std::wstring canonicalTombstone;
    if (!canonicalPath(parent, canonicalParent) || !canonicalPath(tombstone, canonicalTombstone) ||
        !isPathTreeNotReparse(canonicalParent) ||
        !samePath(std::filesystem::path(canonicalTombstone).parent_path().wstring(), canonicalParent) ||
        !validTombstoneName(std::filesystem::path(canonicalTombstone).filename().wstring())) {
        return 0;
    }
    const DWORD tombstoneAttributes = ::GetFileAttributesW(canonicalTombstone.c_str());
    const bool tombstoneExists = tombstoneAttributes != INVALID_FILE_ATTRIBUTES;
    const DWORD tombstoneError = tombstoneExists ? ERROR_SUCCESS : ::GetLastError();
    if (!tombstoneExists && tombstoneError != ERROR_FILE_NOT_FOUND &&
        tombstoneError != ERROR_PATH_NOT_FOUND) return 0;
    const bool hasOwnerSidecar = !ownerSidecar.empty();
    const std::wstring expectedOwnerSidecar = sidecarOwnerPath(canonicalTombstone);
    if (hasOwnerSidecar && (!samePath(ownerSidecar, expectedOwnerSidecar) ||
        !isNotReparsePoint(ownerSidecar, false))) return 0;
    MarkerSnapshot marker;
    if (hasOwnerSidecar) {
        if (!readMarkerFile(ownerSidecar, marker, ignored)) return 0;
    } else if (tombstoneExists) {
        if (!readMarker(canonicalTombstone, marker, ignored)) return 0;
    } else {
        return 0;
    }

    if (!validateReleasedMarkerForParent(canonicalParent, marker, ignored)) return 0;
    if (tombstoneExists && !validateTombstoneMarker(canonicalParent, canonicalTombstone,
        marker, ignored)) return 0;
    const std::wstring ownerSidecarPath = hasOwnerSidecar ? ownerSidecar : expectedOwnerSidecar;
    const std::wstring leaseSidecarPathValue = sidecarLeasePath(canonicalTombstone);

    if (!tombstoneExists) {
        // The directory was removed before the helper could remove its
        // sidecars. The Released marker is the remaining ownership proof.
        HANDLE lease = nullptr;
        const DWORD leaseAttributes = ::GetFileAttributesW(leaseSidecarPathValue.c_str());
        if (leaseAttributes != INVALID_FILE_ATTRIBUTES) {
            if (!isNotReparsePoint(leaseSidecarPathValue, false) ||
                !acquireLeasePath(leaseSidecarPathValue, lease, ignored)) return 0;
        } else {
            const DWORD leaseError = ::GetLastError();
            if (leaseError != ERROR_FILE_NOT_FOUND && leaseError != ERROR_PATH_NOT_FOUND) {
                return 0;
            }
        }
        closeHandle(lease);
        if (!deleteOneSidecar(leaseSidecarPathValue, deadline)) return 2;
        if (!deleteOneSidecar(ownerSidecarPath, deadline)) return 2;
        return 0;
    }

    HANDLE lease = nullptr;
    if (hasOwnerSidecar) {
        const std::wstring leaseInsidePath = canonicalTombstone + L"\\" + kLeaseName;
        const bool leaseInside = isNotReparsePoint(leaseInsidePath, false);
        const bool leaseOutside = isNotReparsePoint(leaseSidecarPathValue, false);
        // Exactly one lease location is acceptable. Both or neither is
        // ambiguous, so leave the tombstone untouched for a later retry.
        if (leaseInside == leaseOutside) return 0;
        const std::wstring& leasePath = leaseOutside ? leaseSidecarPathValue : leaseInsidePath;
        if (!acquireLeasePath(leasePath, lease, ignored)) return 0;
    } else {
        if (!acquireLeaseFile(canonicalTombstone, lease, ignored)) return 0;
    }
    closeHandle(lease);
    if (!moveMetadataToSidecars(canonicalTombstone, deadline, ignored)) return 2;
    if (!deleteTree(canonicalTombstone, deadline, ignored)) {
        // Keep the marked tombstone in place if metadata restoration cannot
        // complete. This preserves the original root identity and nonce for a
        // later bounded recovery pass.
        (void)restoreMetadataFromSidecars(canonicalTombstone, deadline);
        return 2;
    }
    if (!deleteOneSidecar(leaseSidecarPathValue, deadline)) return 2;
    if (!deleteOneSidecar(ownerSidecarPath, deadline)) return 2;
    return 0;
}

int recoverProfiles(const std::wstring& parent)
{
    std::wstring canonicalParent;
    if (!canonicalPath(parent, canonicalParent) || !isPathTreeNotReparse(canonicalParent)) return 2;
    CleanupDeadline deadline(kCleanupDeadlineMs);
    const std::wstring pattern = canonicalParent + L"\\instance-v1-*";
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    std::size_t candidateCount = 0;
    int result = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (deadline.expired()) {
                result = 2;
                break;
            }
            if (++candidateCount > kMaxRecoveryCandidates) {
                result = 2;
                break;
            }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
            const int oneResult = recoverOneProfile(canonicalParent, data.cFileName, deadline);
            if (oneResult != 0) result = oneResult;
        } while (::FindNextFileW(find, &data));
        const DWORD findError = ::GetLastError();
        ::FindClose(find);
        if (findError != ERROR_NO_MORE_FILES) result = 2;
    } else if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
        result = 2;
    }

    const std::wstring tombstonePattern = canonicalParent + L"\\cleanup-v1-*";
    WIN32_FIND_DATAW tombstoneData{};
    HANDLE tombstoneFind = ::FindFirstFileW(tombstonePattern.c_str(), &tombstoneData);
    if (tombstoneFind != INVALID_HANDLE_VALUE) {
        do {
            if (deadline.expired()) {
                result = 2;
                break;
            }
            if (++candidateCount > kMaxRecoveryCandidates) {
                result = 2;
                break;
            }
            if ((tombstoneData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (tombstoneData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                const std::wstring tombstone = canonicalParent + L"\\" + tombstoneData.cFileName;
                const int oneResult = recoverTombstone(canonicalParent, tombstone, L"", deadline);
                if (oneResult != 0) result = oneResult;
            }
        } while (::FindNextFileW(tombstoneFind, &tombstoneData));
        if (::GetLastError() != ERROR_NO_MORE_FILES) result = 2;
        ::FindClose(tombstoneFind);
    } else if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
        result = 2;
    }

    const std::wstring sidecarPattern = canonicalParent + L"\\cleanup-v1-*.owner.bin";
    WIN32_FIND_DATAW sidecarData{};
    HANDLE sidecarFind = ::FindFirstFileW(sidecarPattern.c_str(), &sidecarData);
    if (sidecarFind != INVALID_HANDLE_VALUE) {
        do {
            if (deadline.expired()) {
                result = 2;
                break;
            }
            if (++candidateCount > kMaxRecoveryCandidates) {
                result = 2;
                break;
            }
            if ((sidecarData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                (sidecarData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                const std::wstring sidecar = canonicalParent + L"\\" + sidecarData.cFileName;
                constexpr wchar_t suffix[] = L".owner.bin";
                const std::wstring name(sidecarData.cFileName);
                if (name.size() > std::size(suffix) - 1 &&
                    name.compare(name.size() - (std::size(suffix) - 1),
                        std::size(suffix) - 1, suffix) == 0) {
                    const std::wstring tombstone = sidecar.substr(0,
                        sidecar.size() - (std::size(suffix) - 1));
                    const int oneResult = recoverTombstone(canonicalParent, tombstone, sidecar,
                        deadline);
                    if (oneResult != 0) result = oneResult;
                }
            }
        } while (::FindNextFileW(sidecarFind, &sidecarData));
        if (::GetLastError() != ERROR_NO_MORE_FILES) result = 2;
        ::FindClose(sidecarFind);
    } else if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
        result = 2;
    }
    return result;
}

} // namespace

bool queryProcessIdentity(DWORD processId, ProcessIdentity& identity)
{
    identity = {};
    if (processId == 0) return false;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    const BOOL result = ::GetProcessTimes(process, &identity.creationTime, &exitTime, &kernel, &user);
    ::CloseHandle(process);
    if (!result) {
        identity = {};
        return false;
    }
    identity.processId = processId;
    return true;
}

#ifdef NPPTERMINAL_TESTS
int cleanupProfileForTest(const std::wstring& parentPath,
    const std::wstring& instanceId,
    const std::array<std::uint8_t, kNonceBytes>& nonce,
    std::wstring& error)
{
    error.clear();
    return cleanupProfile(parentPath, instanceId, nonce, error);
}

int cleanupOwnedFixtureForTest(OwnedProfile& profile, std::wstring& error)
{
    error.clear();
    std::wstring canonicalParent;
    std::wstring canonicalRoot;
    if (profile.parentIdentity.empty() || profile.rootPath.empty() ||
        !validInstanceId(profile.instanceId) || !hasNonce(profile.nonce) ||
        !hasFileIdentity(profile.rootFileId) ||
        !canonicalPath(profile.parentIdentity, canonicalParent) ||
        !canonicalPath(profile.rootPath, canonicalRoot) ||
        !samePath(canonicalParent, profile.parentIdentity) ||
        !samePath(canonicalRoot, profile.rootPath) ||
        !samePath(std::filesystem::path(canonicalRoot).parent_path().wstring(),
            canonicalParent) ||
        !isPathTreeNotReparse(canonicalParent) ||
        !isPathTreeNotReparse(canonicalRoot)) {
        error = L"The WebView test fixture identity is invalid or escaped its parent.";
        return 2;
    }

    MarkerSnapshot marker;
    if (!readMarker(profile.rootPath, marker, error) ||
        (marker.phase != CleanupPhase::Closing && marker.phase != CleanupPhase::Released) ||
        !validateRoot(profile.parentIdentity, profile.instanceId, profile.rootPath,
            marker, profile.nonce, error) ||
        !sameFileIdentity(profile.rootFileId, marker.rootFileId)) {
        if (error.empty()) error = L"The WebView test fixture marker is not owned by this profile.";
        return 2;
    }

    // Reopen the exclusive lease through the production validation path. Do
    // not trust an opaque handle supplied inside a test fixture: reacquiring
    // the exact lease path gives this operation a fresh ownership fence.
    releaseOwnedProfileLease(profile);
    if (!acquireOwnedProfileLease(profile, error)) return 2;
    MarkerSnapshot lockedMarker;
    if (!readMarker(profile.rootPath, lockedMarker, error) ||
        (lockedMarker.phase != CleanupPhase::Closing &&
            lockedMarker.phase != CleanupPhase::Released) ||
        !validateRoot(profile.parentIdentity, profile.instanceId, profile.rootPath,
            lockedMarker, profile.nonce, error) ||
        !sameFileIdentity(profile.rootFileId, lockedMarker.rootFileId)) {
        releaseOwnedProfileLease(profile);
        if (error.empty()) error = L"The WebView test fixture changed during lease acquisition.";
        return 2;
    }
    if (lockedMarker.phase == CleanupPhase::Closing &&
        !lockedMarker.processSnapshotComplete) {
        releaseOwnedProfileLease(profile);
        error = L"The WebView test fixture has no complete browser-process snapshot.";
        return 2;
    }

    // The caller supplies the browser-family-exited proof. Preserve the
    // marker's recorded family and completeness bit so the production cleanup
    // path performs its own inactive-identity check before deletion. A Closing
    // marker is never upgraded from an incomplete snapshot by this helper.
    if (!updateOwnedProfile(profile, CleanupPhase::Released,
        lockedMarker.browserProcesses, lockedMarker.processSnapshotComplete, error)) {
        releaseOwnedProfileLease(profile);
        return 2;
    }
    releaseOwnedProfileLease(profile);
    return cleanupProfile(profile.parentIdentity, profile.instanceId, profile.nonce, error);
}
#endif

bool createOwnedProfile(const std::wstring& parentPath,
    const ProcessIdentity& hostProcess, OwnedProfile& profile, std::wstring& error)
{
    profile = {};
    if (hostProcess.processId == 0 ||
        (hostProcess.creationTime.dwHighDateTime == 0 &&
            hostProcess.creationTime.dwLowDateTime == 0)) {
        error = L"The WebView host process identity is invalid.";
        return false;
    }
    std::wstring parent;
    if (!canonicalPath(parentPath, parent) || !ensureParentDirectory(parent, error) ||
        !canonicalPath(parent, parent)) return false;
    if (parent.size() >= kMaxParentIdentityChars) {
        error = L"The WebView profile parent path is longer than the marker schema allows.";
        return false;
    }

    bool created = false;
    for (unsigned int attempt = 0; attempt != 4 && !created; ++attempt) {
        std::array<std::uint8_t, kNonceBytes> nonce{};
        std::wstring instanceId;
        if (!makeInstanceId(instanceId, nonce)) {
            error = L"Unable to generate a cryptographically random WebView profile identity.";
            return false;
        }
        const std::wstring root = parent + L"\\" + instanceId;
        if (::CreateDirectoryW(root.c_str(), nullptr)) {
            profile.rootPath = root;
            profile.instanceId = instanceId;
            profile.parentIdentity = parent;
            profile.nonce = nonce;
            profile.hostProcess = hostProcess;
            profile.phase = CleanupPhase::Active;
            if (!getFileIdentity(root, profile.rootFileId, true) ||
                !writeMarker(profile, true, error)) {
                ::DeleteFileW((root + L"\\" + kOwnerMarkerName).c_str());
                ::RemoveDirectoryW(root.c_str());
                profile = {};
                return false;
            }
            profile.lease = ::CreateFileW((root + L"\\" + kLeaseName).c_str(),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (!validHandle(profile.lease)) {
                const DWORD openError = ::GetLastError();
                profile.lease = nullptr;
                error = L"Unable to acquire the WebView profile lease: " + win32Error(openError);
                ::DeleteFileW((root + L"\\" + kOwnerMarkerName).c_str());
                ::RemoveDirectoryW(root.c_str());
                profile = {};
                return false;
            }
            created = true;
        } else if (::GetLastError() != ERROR_ALREADY_EXISTS) {
            error = L"Unable to create the WebView profile directory: " + win32Error();
            return false;
        }
    }
    if (!created) {
        error = L"Unable to allocate a unique WebView profile directory.";
        return false;
    }
    return true;
}

bool updateOwnedProfile(OwnedProfile& profile, CleanupPhase phase,
    const std::vector<ProcessIdentity>& browserProcesses,
    bool processSnapshotComplete, std::wstring& error)
{
    if (!profile.lease || !validInstanceId(profile.instanceId) || profile.rootPath.empty() ||
        browserProcesses.size() > kMaxTrackedProcesses ||
        (phase != CleanupPhase::Active && phase != CleanupPhase::Closing &&
            phase != CleanupPhase::Released)) {
        error = L"The WebView profile update is invalid or has no exclusive lease.";
        return false;
    }
    if (!browserProcesses.empty()) {
        for (const ProcessIdentity& process : browserProcesses) {
            if (process.processId == 0) {
                error = L"The WebView process family contains an invalid identity.";
                return false;
            }
        }
    }
    OwnedProfile updated = profile;
    updated.phase = phase;
    updated.browserProcesses = browserProcesses;
    updated.processSnapshotComplete = processSnapshotComplete;
    if (!writeMarker(updated, false, error)) return false;
    profile = std::move(updated);
    return true;
}

bool acquireOwnedProfileLease(OwnedProfile& profile, std::wstring& error)
{
    if (profile.lease) return true;
    if (!validInstanceId(profile.instanceId) || profile.rootPath.empty() ||
        !isNotReparsePoint(profile.rootPath, true)) {
        error = L"The WebView profile lease target is invalid.";
        return false;
    }
    MarkerSnapshot marker;
    if (!readMarker(profile.rootPath, marker, error) ||
        (marker.phase != CleanupPhase::Closing && marker.phase != CleanupPhase::Released) ||
        marker.instanceId != profile.instanceId ||
        !std::equal(profile.nonce.begin(), profile.nonce.end(), marker.nonce.begin()) ||
        !samePath(marker.parentIdentity, profile.parentIdentity) ||
        !sameFileIdentity(marker.rootFileId, profile.rootFileId)) {
        error = L"The WebView profile is not in a releasable marker phase.";
        return false;
    }
    HANDLE lease = nullptr;
    if (!acquireLeaseFile(profile.rootPath, lease, error)) return false;
    profile.lease = lease;
    return true;
}

void releaseOwnedProfileLease(OwnedProfile& profile)
{
    closeHandle(profile.lease);
}

bool launchCleanupHelper(const std::wstring& moduleDirectory,
    const OwnedProfile& profile, std::wstring& error)
{
    if (!validInstanceId(profile.instanceId) || profile.rootPath.empty() ||
        profile.nonce == std::array<std::uint8_t, kNonceBytes>{}) {
        error = L"The WebView cleanup identity is invalid.";
        return false;
    }
    std::wstring module;
    if (!canonicalPath(moduleDirectory, module) || !isPathTreeNotReparse(module)) {
        error = L"The plugin module directory is invalid.";
        return false;
    }
    const std::wstring helper = module + L"\\NppTerminalBroker.exe";
    const DWORD attributes = ::GetFileAttributesW(helper.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !helperVersionMatches(helper, error)) return false;
    const std::wstring command = L"\"" + helper + L"\" --cleanup \"" +
        profile.instanceId + L"\" " + nonceHex(profile.nonce);
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    const BOOL launched = ::CreateProcessW(helper.c_str(), commandLine.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, module.c_str(), &startup, &processInfo);
    if (!launched) {
        error = L"Unable to launch the WebView cleanup helper: " + win32Error();
        return false;
    }
    ::CloseHandle(processInfo.hThread);
    ::CloseHandle(processInfo.hProcess);
    return true;
}

bool launchRecoveryHelper(const std::wstring& moduleDirectory, std::wstring& error)
{
    std::wstring module;
    if (!canonicalPath(moduleDirectory, module) || !isPathTreeNotReparse(module)) {
        error = L"The plugin module directory is invalid.";
        return false;
    }
    const std::wstring helper = module + L"\\NppTerminalBroker.exe";
    const DWORD attributes = ::GetFileAttributesW(helper.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !helperVersionMatches(helper, error)) return false;
    const std::wstring command = L"\"" + helper + L"\" --cleanup-recover";
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    const BOOL launched = ::CreateProcessW(helper.c_str(), commandLine.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, module.c_str(), &startup, &processInfo);
    if (!launched) {
        error = L"Unable to launch the WebView cleanup recovery helper: " + win32Error();
        return false;
    }
    ::CloseHandle(processInfo.hThread);
    ::CloseHandle(processInfo.hProcess);
    return true;
}

int runProfileCleanupMode(int argc, wchar_t** argv, bool& handled)
{
    handled = false;
    if (argc <= 1 || !argv || !argv[1]) return 0;
#ifdef NPPTERMINAL_TESTS
    std::wstring fixtureParent;
    if (_wcsicmp(argv[1], L"--cleanup-test-parent") == 0) {
        handled = true;
        if (argc < 4 || !argv[2]) return 2;
        std::wstring localData;
        if (!getLocalAppData(localData) || !canonicalPath(argv[2], fixtureParent) ||
            !isPathTreeNotReparse(fixtureParent)) return 2;
        const std::filesystem::path fixturePath(fixtureParent);
        const std::wstring name = fixturePath.filename().wstring();
        const std::wstring prefix = L"NppTerminal.ProfileCleanup.Tests-";
        GUID guid{};
        if (!samePath(fixturePath.parent_path().wstring(), localData) ||
            name.rfind(prefix, 0) != 0 ||
            FAILED(::CLSIDFromString((L"{" + name.substr(prefix.size()) + L"}").c_str(), &guid))) {
            return 2;
        }
        argc -= 2;
        argv += 2;
    }
#endif
    const bool cleanupMode = _wcsicmp(argv[1], L"--cleanup") == 0;
    const bool recoveryMode = _wcsicmp(argv[1], L"--cleanup-recover") == 0;
    if (!cleanupMode && !recoveryMode) {
#ifdef NPPTERMINAL_TESTS
        if (!fixtureParent.empty()) return 2;
#endif
        return 0;
    }
    handled = true;
    if ((cleanupMode &&
        (argc != 4 || !argv[2] || !argv[3])) || (recoveryMode && argc != 2)) return 2;

    // This watchdog exists only in the disposable helper process.  It keeps a
    // filesystem API stall from turning into a permanent background process;
    // the DLL never calls this entrypoint and creates no cleanup thread.
    std::atomic_bool finished{false};
    HANDLE finishedEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog;
    try {
        if (finishedEvent) {
            const HANDLE eventForWatchdog = finishedEvent;
            watchdog = std::thread([&finished, eventForWatchdog] {
                if (::WaitForSingleObject(eventForWatchdog, kCleanupWatchdogMs) == WAIT_TIMEOUT &&
                    !finished.exchange(true)) {
                    ::TerminateProcess(::GetCurrentProcess(), ERROR_TIMEOUT);
                }
            });
        }
    } catch (...) {
        // Refuse cleanup if the disposable helper cannot establish its hard
        // process deadline. This keeps a filesystem stall from becoming an
        // unbounded cleanup process.
    }
    if (!watchdog.joinable()) {
        if (finishedEvent) ::CloseHandle(finishedEvent);
        return 2;
    }

    const int result = [&]() -> int {
        std::wstring localData;
        if (!getLocalAppData(localData)) return 2;
        std::wstring parent = localData + L"\\NppTerminal\\WebView";
#ifdef NPPTERMINAL_TESTS
        if (!fixtureParent.empty()) parent = fixtureParent;
#endif
        std::wstring error;
        if (recoveryMode) return recoverProfiles(parent);
        const std::wstring instanceId(argv[2]);
        std::array<std::uint8_t, kNonceBytes> nonce{};
        if (!validInstanceId(instanceId) || !parseNonce(argv[3], nonce)) return 2;
        return cleanupProfile(parent, instanceId, nonce, error);
    }();
    finished.store(true);
    if (finishedEvent) ::SetEvent(finishedEvent);
    if (watchdog.joinable()) watchdog.join();
    if (finishedEvent) ::CloseHandle(finishedEvent);
    return result;
}

} // namespace nppterminal::profilecleanup
