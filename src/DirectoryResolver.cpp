#include "DirectoryResolver.h"

#include "ShellCatalog.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <utility>

namespace nppterminal {

namespace {

std::wstring normalizedPath(const std::wstring& value)
{
    if (!isAbsoluteWindowsPath(value)) return {};
    DWORD required = ::GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
    if (required == 0) return {};
    std::wstring result(required, L'\0');
    const DWORD length = ::GetFullPathNameW(value.c_str(), required, result.data(), nullptr);
    if (length == 0 || length >= required) return {};
    result.resize(length);
    return result;
}

bool defaultProbe(const std::wstring& value)
{
    const DWORD attributes = ::GetFileAttributesW(value.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool usableCandidate(const std::wstring& value, bool rejectUncForCmd,
    const DirectoryProbe& probe, std::wstring& normalized)
{
    normalized = normalizedPath(value);
    if (normalized.empty()) return false;
    if (rejectUncForCmd && isUncWindowsPath(normalized)) return false;
    return probe ? probe(normalized) : false;
}

} // namespace

bool resolveWorkingDirectoryWithProbe(const DirectoryCandidates& candidates,
    bool rejectUncForCmd, const DirectoryProbe& probe,
    std::wstring& workingDirectory, std::wstring& error)
{
    workingDirectory.clear();
    error.clear();
    if (!candidates.explicitDirectory.empty()) {
        std::wstring normalized;
        if (!usableCandidate(candidates.explicitDirectory, rejectUncForCmd, probe, normalized)) {
            error = L"The explicit terminal directory is invalid or unsupported: " +
                candidates.explicitDirectory;
            return false;
        }
        workingDirectory = std::move(normalized);
        return true;
    }

    const std::wstring automatic[] = {
        candidates.activeFileDirectory,
        candidates.defaultDirectory,
        candidates.workingDirectory,
        candidates.homeDirectory,
    };
    for (const std::wstring& candidate : automatic) {
        std::wstring normalized;
        if (usableCandidate(candidate, rejectUncForCmd, probe, normalized)) {
            workingDirectory = std::move(normalized);
            return true;
        }
    }
    error = L"No usable terminal working directory was found.";
    return false;
}

bool resolveWorkingDirectory(const DirectoryCandidates& candidates,
    bool rejectUncForCmd, std::wstring& workingDirectory, std::wstring& error)
{
    return resolveWorkingDirectoryWithProbe(candidates, rejectUncForCmd,
        defaultProbe, workingDirectory, error);
}

} // namespace nppterminal
