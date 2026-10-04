#pragma once

#include <string>
#include <vector>

namespace nppterminal {

struct ShellInfo {
    std::wstring id;
    std::wstring displayName;
    std::wstring applicationName;
};

// Detects shells already installed on this Windows host.  The implementation
// only checks known installation locations, registry install locations, and
// explicit absolute directories from PATH; it never searches the current
// directory or the plugin/project directory implicitly.
bool discoverShellCatalog(std::vector<ShellInfo>& catalog, std::wstring& error);

// Runs only the bounded, read-only WSL capability probes used by catalog
// discovery.  It never installs, terminates, unregisters, or shuts down WSL.
bool verifyWslCapability(const std::wstring& applicationName, std::wstring& error);

bool isAbsoluteWindowsPath(const std::wstring& path);
bool isUncWindowsPath(const std::wstring& path);

} // namespace nppterminal
