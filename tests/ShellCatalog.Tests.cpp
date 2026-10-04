#include "ShellCatalog.Tests.h"

#include "DirectoryResolver.h"
#include "ShellCatalog.h"
#include "ShellDiscovery.h"

#include <stdexcept>
#include <string>

namespace nppterminal::tests {

namespace {

[[noreturn]] void fail(const char* message)
{
    throw std::runtime_error(message);
}

void require(bool condition, const char* message)
{
    if (!condition) fail(message);
}

void testAbsolutePathClassification()
{
    require(isAbsoluteWindowsPath(L"C:\\Windows"), "drive path was not absolute");
    require(isAbsoluteWindowsPath(L"\\\\server\\share"), "UNC path was not absolute");
    require(!isAbsoluteWindowsPath(L"Windows\\System32"), "relative path was accepted");
    require(!isAbsoluteWindowsPath(L"C:Windows"), "drive-relative path was accepted");
    require(isUncWindowsPath(L"\\\\server\\share"), "UNC path was not classified");
    require(!isUncWindowsPath(L"C:\\Windows"), "drive path was classified as UNC");
}

void testDirectoryPriorityAndFallback()
{
    DirectoryCandidates candidates;
    candidates.explicitDirectory = L"C:\\explicit";
    candidates.activeFileDirectory = L"C:\\active";
    candidates.defaultDirectory = L"C:\\default";
    candidates.workingDirectory = L"C:\\working";
    candidates.homeDirectory = L"C:\\home";
    std::wstring selected;
    std::wstring error;
    require(resolveWorkingDirectoryWithProbe(candidates, false,
        [](const std::wstring& path) { return path == L"C:\\explicit"; }, selected, error) &&
        selected == L"C:\\explicit", "explicit directory did not win priority");

    candidates.explicitDirectory.clear();
    require(resolveWorkingDirectoryWithProbe(candidates, false,
        [](const std::wstring& path) { return path == L"C:\\default"; }, selected, error) &&
        selected == L"C:\\default", "automatic directory fallback order failed");

    candidates.explicitDirectory = L"relative\\directory";
    require(!resolveWorkingDirectoryWithProbe(candidates, false,
        [](const std::wstring&) { return true; }, selected, error) &&
        error.find(L"relative\\directory") != std::wstring::npos,
        "invalid explicit directory did not produce an actionable error");
}

void testCmdUncPolicy()
{
    DirectoryCandidates candidates;
    candidates.activeFileDirectory = L"\\\\server\\share\\project";
    candidates.homeDirectory = L"C:\\Users\\tester";
    std::wstring selected;
    std::wstring error;
    require(resolveWorkingDirectoryWithProbe(candidates, true,
        [](const std::wstring&) { return true; }, selected, error) &&
        selected == candidates.homeDirectory,
        "cmd did not skip an automatic UNC directory for a usable fallback");
    candidates.explicitDirectory = candidates.activeFileDirectory;
    require(!resolveWorkingDirectoryWithProbe(candidates, true,
        [](const std::wstring&) { return true; }, selected, error),
        "cmd accepted an explicit UNC working directory");
    candidates.explicitDirectory.clear();
    require(resolveWorkingDirectoryWithProbe(candidates, false,
        [](const std::wstring& path) { return path == L"\\\\server\\share\\project"; },
        selected, error) && selected == L"\\\\server\\share\\project",
        "non-cmd shell rejected a usable UNC working directory");
}

void testShellLaunchPlans()
{
    std::wstring command;
    std::wstring error;
    bool preserveDirectory = false;
    const std::wstring directory = L"C:\\Work Area";

    ShellInfo powershell{L"powershell7", L"PowerShell 7",
        L"C:\\Program Files\\PowerShell\\7\\pwsh.exe"};
    require(buildShellLaunchCommand(powershell, directory, command, preserveDirectory, error) &&
        command == L"\"C:\\Program Files\\PowerShell\\7\\pwsh.exe\" -NoLogo" &&
        !preserveDirectory, "PowerShell launch plan was malformed");

    ShellInfo gitBash{L"gitbash", L"Git Bash", L"C:\\Program Files\\Git\\bin\\bash.exe"};
    require(buildShellLaunchCommand(gitBash, directory, command, preserveDirectory, error) &&
        command == L"\"C:\\Program Files\\Git\\bin\\bash.exe\" --login -i" &&
        preserveDirectory, "Git Bash launch plan did not request directory preservation");

    ShellInfo wsl{L"wsl", L"WSL", L"C:\\Windows\\System32\\wsl.exe"};
    require(buildShellLaunchCommand(wsl, directory, command, preserveDirectory, error) &&
        command == L"C:\\Windows\\System32\\wsl.exe --cd \"C:\\Work Area\"" &&
        !preserveDirectory, "WSL launch plan was malformed");

    ShellInfo unsupported{L"zsh", L"Unsupported", L"C:\\zsh.exe"};
    require(!buildShellLaunchCommand(unsupported, directory, command, preserveDirectory, error) &&
        !error.empty(), "unsupported shell ID was accepted");
}

} // namespace

void runShellCatalogTests()
{
    testAbsolutePathClassification();
    testDirectoryPriorityAndFallback();
    testCmdUncPolicy();
    testShellLaunchPlans();
}

} // namespace nppterminal::tests
