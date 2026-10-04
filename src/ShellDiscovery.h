#pragma once

#include "DirectoryResolver.h"
#include "ShellCatalog.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nppterminal {

struct ShellDiscoveryRequest {
    bool refreshCatalog = false;
    std::vector<ShellInfo> cachedCatalog;
    std::wstring requestedShellId;
    // True when requestedShellId is an explicit user choice.  An explicit
    // missing shell is an error; the default preference may fall back.
    bool explicitShell = false;
    DirectoryCandidates candidates;
};

struct ShellDiscoveryResult {
    std::vector<ShellInfo> catalog;
    std::wstring applicationName;
    std::wstring commandLine;
    std::wstring workingDirectory;
    bool gitBashPreserveDirectory = false;
    std::wstring error;
};

constexpr std::size_t kShellDiscoveryMaxJsonBytes = 64u * 1024u;
constexpr std::uint32_t kShellDiscoveryWslTimeoutMs = 5000;
constexpr std::uint32_t kShellDiscoveryOverallTimeoutMs = 10000;

bool buildShellLaunchCommand(const ShellInfo& shell, const std::wstring& workingDirectory,
    std::wstring& commandLine, bool& gitBashPreserveDirectory, std::wstring& error);

// The DLL-side client owns no worker thread.  The helper process performs all
// shell detection, WSL probing, and filesystem validation; the caller polls
// this facade from its UI/event loop.
class ShellDiscoveryClient final {
public:
    ShellDiscoveryClient();
    ~ShellDiscoveryClient();

    ShellDiscoveryClient(const ShellDiscoveryClient&) = delete;
    ShellDiscoveryClient& operator=(const ShellDiscoveryClient&) = delete;

    bool start(const ShellDiscoveryRequest& request, std::wstring& error);
    void poll();
    bool finished() const;
    const ShellDiscoveryResult& result() const;
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// BrokerMain calls this before normal IPC argument handling.  It returns the
// helper exit code and sets handled for the exact --discover mode only.
int runShellDiscoveryMode(int argc, wchar_t** argv, bool& handled);

} // namespace nppterminal
