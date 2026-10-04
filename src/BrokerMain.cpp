#include "BrokerSession.h"
#include "ProfileCleanup.h"
#include "ShellDiscovery.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <string>

namespace {

std::uintptr_t handleArgument(const std::wstring& commandLine, const wchar_t* name)
{
    const std::wstring prefix = std::wstring(name) + L"=";
    const std::size_t begin = commandLine.find(prefix);
    if (begin == std::wstring::npos) return 0;
    const std::size_t valueBegin = begin + prefix.size();
    std::size_t valueEnd = commandLine.find_first_of(L" \t\r\n", valueBegin);
    if (valueEnd == std::wstring::npos) valueEnd = commandLine.size();
    try {
        std::size_t parsed = 0;
        const std::uintptr_t value = static_cast<std::uintptr_t>(
            std::stoull(commandLine.substr(valueBegin, valueEnd - valueBegin), &parsed, 10));
        return parsed == valueEnd - valueBegin ? value : 0;
    } catch (...) {
        return 0;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argumentCount = 0;
    LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
    if (!arguments) return 2;
    bool cleanupMode = false;
    const int cleanupResult = nppterminal::profilecleanup::runProfileCleanupMode(
        argumentCount, arguments, cleanupMode);
    if (cleanupMode) {
        ::LocalFree(arguments);
        return cleanupResult;
    }

    bool discoveryMode = false;
    const int discoveryResult = nppterminal::runShellDiscoveryMode(
        argumentCount, arguments, discoveryMode);
    ::LocalFree(arguments);
    if (discoveryMode) return discoveryResult;

    const std::wstring commandLine = ::GetCommandLineW();
    const std::uintptr_t commandValue = handleArgument(commandLine, L"--command-handle");
    const std::uintptr_t outputValue = handleArgument(commandLine, L"--output-handle");
    const std::uintptr_t eventValue = handleArgument(commandLine, L"--event-handle");
    if (commandValue == 0 || outputValue == 0 || eventValue == 0) return 2;

    nppterminal::BrokerSession session(
        reinterpret_cast<HANDLE>(commandValue),
        reinterpret_cast<HANDLE>(outputValue),
        reinterpret_cast<HANDLE>(eventValue));
    return session.run();
}
