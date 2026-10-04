#pragma once

#include <functional>
#include <string>

namespace nppterminal {

struct DirectoryCandidates {
    std::wstring explicitDirectory;
    std::wstring activeFileDirectory;
    std::wstring defaultDirectory;
    std::wstring workingDirectory;
    std::wstring homeDirectory;
};

using DirectoryProbe = std::function<bool(const std::wstring&)>;

// Selects explicit -> active file -> configured default -> captured host
// working directory -> home.  The explicit path is actionable when invalid;
// invalid automatic candidates fall through.  The probe overload keeps the
// selection policy deterministic and fixture-testable.
bool resolveWorkingDirectory(const DirectoryCandidates& candidates,
    bool rejectUncForCmd, std::wstring& workingDirectory, std::wstring& error);
bool resolveWorkingDirectoryWithProbe(const DirectoryCandidates& candidates,
    bool rejectUncForCmd, const DirectoryProbe& probe,
    std::wstring& workingDirectory, std::wstring& error);

} // namespace nppterminal
