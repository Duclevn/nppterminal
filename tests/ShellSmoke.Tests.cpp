#include "ShellSmoke.Tests.h"

#include "Protocol.h"
#include "ShellDiscovery.h"
#include "TerminalSession.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace nppterminal::tests {
namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error("SHELL_SMOKE: " + message);
}

// ConPTY may insert VT cursor/colour controls or physical line wraps into a
// long printed directory. Match the controlled marker after removing those
// presentation controls; this fixture is not a terminal emulator.
std::string printedText(const std::string& bytes)
{
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(bytes[i]);
        if (ch == 0x1b && i + 1 < bytes.size()) {
            if (bytes[i + 1] == '[') {
                i += 2;
                while (i < bytes.size() && (bytes[i] < 0x40 || bytes[i] > 0x7e)) ++i;
                continue;
            }
            if (bytes[i + 1] == ']') {
                i += 2;
                while (i < bytes.size() && bytes[i] != '\a') {
                    if (bytes[i] == 0x1b && i + 1 < bytes.size() && bytes[i + 1] == '\\') {
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            ++i;
            continue;
        }
        if (ch >= 0x20 && ch != 0x7f) text.push_back(static_cast<char>(ch));
    }
    return text;
}

struct DirectoryFixture final {
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};

    DirectoryFixture()
    {
        wchar_t parent[32768]{};
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(std::size(parent)), parent);
        require(length != 0 && length < std::size(parent), "temporary parent unavailable");
        GUID guid{};
        require(SUCCEEDED(::CoCreateGuid(&guid)), "GUID unavailable");
        wchar_t text[64]{};
        require(::StringFromGUID2(guid, text, static_cast<int>(std::size(text))) != 0,
            "GUID formatting failed");
        path = std::wstring(parent, length) + L"NppTerminal Shell Tests \u65e5\u672c " + text;
        require(::CreateDirectoryW(path.c_str(), nullptr) != FALSE, "fixture creation failed");
        handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(handle != INVALID_HANDLE_VALUE &&
            ::GetFileInformationByHandle(handle, &identity) &&
            !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "fixture identity unavailable");
    }

    ~DirectoryFixture()
    {
        if (handle == INVALID_HANDLE_VALUE) return;
        HANDLE current = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION now{};
        const bool safe = current != INVALID_HANDLE_VALUE &&
            ::GetFileInformationByHandle(current, &now) &&
            !(now.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            now.dwVolumeSerialNumber == identity.dwVolumeSerialNumber &&
            now.nFileIndexHigh == identity.nFileIndexHigh && now.nFileIndexLow == identity.nFileIndexLow;
        if (current != INVALID_HANDLE_VALUE) ::CloseHandle(current);
        ::CloseHandle(handle);
        // Exact empty-directory removal only. Startup profiles may create files;
        // leave those in place rather than recursively deleting unknown data.
        if (safe) ::RemoveDirectoryW(path.c_str());
    }
};

struct ProcessHandle final {
    HANDLE handle = nullptr;
    explicit ProcessHandle(DWORD pid)
        : handle(::OpenProcess(SYNCHRONIZE, FALSE, pid)) {}
    ~ProcessHandle() { if (handle) ::CloseHandle(handle); }
    bool stopped() const { return handle && ::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0; }
};

struct LinuxProcess final {
    std::string kind;
    std::string pid;
    std::string start;
};

std::wstring quoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

template <typename Predicate>
bool waitFor(TerminalSession& session, Predicate predicate, DWORD timeout)
{
    const ULONGLONG deadline = ::GetTickCount64() + timeout;
    do {
        session.poll();
        if (predicate()) return true;
        ::Sleep(5);
    } while (::GetTickCount64() < deadline);
    session.poll();
    return predicate();
}

std::string runWslProbe(const ShellDiscoveryResult& launch, const std::string& script)
{
    std::string output;
    bool overflow = false;
    SessionCallbacks callbacks;
    callbacks.output = [&](std::uint64_t, std::uint64_t,
        const std::vector<std::uint8_t>& bytes, const std::atomic_bool&) {
        if (output.size() + bytes.size() <= 65536) {
            output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        } else overflow = true;
        return true;
    };
    TerminalSession probe(std::move(callbacks));
    SessionStartOptions options;
    options.applicationName = launch.applicationName;
    options.commandLine = quoteArgument(launch.applicationName) +
        L" --exec /bin/sh -c " + quoteArgument(utf8ToWide(script));
    options.workingDirectory = launch.workingDirectory;
    std::wstring error;
    require(probe.start(options, error), "WSL identity probe start: " + wideToUtf8(error));
    require(waitFor(probe, [&] { return !probe.hasProcess() && !probe.hasPendingWork(); }, 10000),
        "bounded WSL identity probe did not finish");
    require(!overflow && probe.state() == SessionState::Exited, "WSL identity probe failed");
    return printedText(output);
}

std::string identityScript(const std::vector<LinuxProcess>& processes, bool cleanup)
{
    std::string script;
    for (const auto& process : processes) {
        // PIDs/start ticks are restricted to decimal digits by the fixture
        // parser. A reused PID must never be treated as our recorded process.
        script += "if [ -d /proc/" + process.pid + " ]; then ";
        script += "s=$(cut -d ' ' -f22 /proc/" + process.pid + "/stat 2>/dev/null); ";
        script += "if [ -z \"$s\" ]; then printf 'NPP_UNKNOWN|" + process.kind + "\\n'; ";
        script += "elif [ \"$s\" = '" + process.start + "' ]; then ";
        if (cleanup) script += "kill -KILL " + process.pid + " 2>/dev/null; ";
        else script += "printf 'NPP_ALIVE|" + process.kind + "\\n'; ";
        script += "else printf 'NPP_GONE|" + process.kind + "\\n'; fi; ";
        script += "else printf 'NPP_GONE|" + process.kind + "\\n'; fi; ";
    }
    return script;
}

ShellDiscoveryResult discover(const std::wstring& shellId, const std::wstring& directory,
    const std::vector<ShellInfo>& cached)
{
    ShellDiscoveryRequest request;
    request.requestedShellId = shellId;
    request.explicitShell = true;
    request.cachedCatalog = cached;
    request.candidates.explicitDirectory = directory;
    ShellDiscoveryClient client;
    std::wstring error;
    require(client.start(request, error), "discovery start: " + wideToUtf8(error));
    const ULONGLONG deadline = ::GetTickCount64() + 12000;
    while (!client.finished() && ::GetTickCount64() < deadline) {
        client.poll();
        ::Sleep(5);
    }
    require(client.finished(), "discovery did not finish");
    require(client.result().error.empty(), "discovery: " + wideToUtf8(client.result().error));
    return client.result();
}

void testShell(const std::wstring& id, const ShellDiscoveryResult& launch,
    const std::wstring& directory)
{
    std::string output;
    bool overflow = false;
    std::size_t cursorRequests = 0;
    std::size_t cursorReplies = 0;
    std::size_t cursorScan = 0;
    bool deviceAttributesReplied = false;
    std::wstring terminalError;
    SessionCallbacks callbacks;
    callbacks.output = [&](std::uint64_t, std::uint64_t,
        const std::vector<std::uint8_t>& bytes, const std::atomic_bool&) {
        if (output.size() + bytes.size() <= 2u * 1024u * 1024u) {
            output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            for (;;) {
                const auto found = output.find("\x1b[6n", cursorScan);
                if (found == std::string::npos) break;
                ++cursorRequests;
                cursorScan = found + 4;
            }
        } else overflow = true;
        return true;
    };
    callbacks.state = [&](std::uint64_t, SessionState state, const std::wstring& message) {
        if (state == SessionState::Error) terminalError = message;
    };
    TerminalSession session(std::move(callbacks));
    const auto replyToTerminalQueries = [&] {
        if (!deviceAttributesReplied && output.find("\x1b[c") != std::string::npos) {
            const std::string response = "\x1b[?1;2c";
            if (session.write(std::vector<std::uint8_t>(response.begin(), response.end()))) {
                deviceAttributesReplied = true;
            }
        }
        while (cursorReplies < cursorRequests) {
            // PSReadLine asks xterm for its cursor position during startup.
            // This native transport fixture supplies a bounded fixed response;
            // the separate real-xterm fixture validates browser parsing.
            const std::string response = "\x1b[1;1R";
            if (!session.write(std::vector<std::uint8_t>(response.begin(), response.end()))) break;
            ++cursorReplies;
        }
    };
    SessionStartOptions options;
    options.applicationName = launch.applicationName;
    options.commandLine = launch.commandLine;
    options.workingDirectory = launch.workingDirectory;
    options.gitBashPreserveDirectory = launch.gitBashPreserveDirectory;
    options.columns = 500;
    std::wstring error;
    require(session.start(options, error), "start " + wideToUtf8(id) + ": " + wideToUtf8(error));
    require(waitFor(session, [&] { replyToTerminalQueries(); return session.state() == SessionState::Running ||
        session.state() == SessionState::Error; }, 15000) && session.state() == SessionState::Running,
        "running " + wideToUtf8(id) + ": " + wideToUtf8(terminalError));
    ProcessHandle broker(session.brokerProcessId());
    ProcessHandle shell(session.processId());
    require(broker.handle && shell.handle, "owned process handles unavailable");
    std::string command;
    if (id == L"cmd") command = "echo [NPP_CWD]%CD%[/NPP_CWD]\r";
    else if (id == L"powershell7") command =
        "Write-Output ('[NPP_CWD]' + (Get-Location).Path + '[/NPP_CWD]')\r";
    else if (id == L"gitbash") command =
        "printf '\\n[NPP_CWD]%s[/NPP_CWD]\\n' \"$(cygpath -w \"$PWD\")\"\r";
    else command = "printf '\\n[NPP_CWD]%s[/NPP_CWD]\\n' \"$(wslpath -w \"$PWD\")\"\r";
    require(session.write(std::vector<std::uint8_t>(command.begin(), command.end())), "input rejected");
    const std::string expected = "[NPP_CWD]" + wideToUtf8(directory) + "[/NPP_CWD]";
    ULONGLONG nextQuery = ::GetTickCount64() + 500;
    unsigned queryCount = 1;
    const bool cwdObserved = waitFor(session, [&] {
        replyToTerminalQueries();
        if (::GetTickCount64() >= nextQuery && queryCount < 3 &&
            printedText(output).find(expected) == std::string::npos) {
            // Running means process creation, before a user startup profile or
            // line editor necessarily accepts input. Retry only this fixed,
            // read-only query; never retry arbitrary user commands.
            if (session.write(std::vector<std::uint8_t>(command.begin(), command.end()))) ++queryCount;
            nextQuery = ::GetTickCount64() + 500;
        }
        return overflow || !terminalError.empty() ||
        printedText(output).find(expected) != std::string::npos; }, 15000) &&
        !overflow && terminalError.empty() && printedText(output).find(expected) != std::string::npos;
    if (!cwdObserved) {
        const std::string visible = printedText(output);
        std::size_t begin = 0;
        while ((begin = visible.find("[NPP_CWD]", begin)) != std::string::npos) {
            begin += 9;
            const auto end = visible.find("[/NPP_CWD]", begin);
            if (end != std::string::npos && end - begin < 1024 &&
                begin + 2 < visible.size() && visible[begin + 1] == ':') {
                std::cout << "SHELL_SMOKE cwd-result=" << visible.substr(begin, end - begin) << '\n';
                break;
            }
        }
        std::cout << "SHELL_SMOKE diagnostic shell=" << wideToUtf8(id)
            << " executable=" << wideToUtf8(launch.applicationName)
            << " state=" << static_cast<int>(session.state()) << " outputBytes=" << output.size()
            << " cursorRequests=" << cursorRequests << " cursorReplies=" << cursorReplies
            << " markerSeen=" << (output.find("[NPP_CWD]") != std::string::npos)
            << " queryCount=" << queryCount
            << " win32Input=" << (output.find("\x1b[?9001h") != std::string::npos)
            << " error=" << wideToUtf8(terminalError) << '\n';
    }
    require(cwdObserved, "interactive Unicode/space cwd not proven for " + wideToUtf8(id));
    require(session.resize(100, 30), "resize rejected");
    std::vector<LinuxProcess> linuxProcesses;
    struct LinuxCleanupGuard final {
        const ShellDiscoveryResult& launch;
        const std::vector<LinuxProcess>& processes;
        bool verified = false;
        ~LinuxCleanupGuard()
        {
            if (verified || processes.empty()) return;
            try { (void)runWslProbe(launch, identityScript(processes, true)); }
            catch (...) { std::cerr << "SHELL_SMOKE owned Linux fixture cleanup unverified\n"; }
        }
    } linuxGuard{launch, linuxProcesses};
    if (id == L"wsl") {
        const std::string program = R"SH(printf '\nNPP_OWNER|shell|%s|%s\n' "$$" "$(cut -d ' ' -f22 /proc/$$/stat)"; sleep 300 & b=$!; printf '\nNPP_OWNER|background|%s|%s\n' "$b" "$(cut -d ' ' -f22 /proc/$b/stat)"; sh -c 'printf "\nNPP_OWNER|foreground|%s|%s\n" "$$" "$(cut -d " " -f22 /proc/$$/stat)"; exec sleep 300')SH";
        const std::string input = program + "\r";
        require(session.write(std::vector<std::uint8_t>(input.begin(), input.end())), "WSL fixture input rejected");
        const std::regex marker("NPP_OWNER\\|(shell|background|foreground)\\|([0-9]{1,10})\\|([0-9]{1,20})");
        require(waitFor(session, [&] {
            replyToTerminalQueries();
            const std::string visible = printedText(output);
            for (auto match = std::sregex_iterator(visible.begin(), visible.end(), marker);
                match != std::sregex_iterator(); ++match) {
                const std::string kind = (*match)[1];
                const auto existing = std::find_if(linuxProcesses.begin(), linuxProcesses.end(),
                    [&](const LinuxProcess& process) { return process.kind == kind; });
                if (existing == linuxProcesses.end()) {
                    const std::string pid = (*match)[2];
                    const std::string start = (*match)[3];
                    if (std::stoull(pid) > 1 && std::stoull(start) != 0) {
                        linuxProcesses.push_back(LinuxProcess{kind, pid, start});
                    }
                }
            }
            return linuxProcesses.size() == 3;
        }, 10000), "WSL attached shell/foreground/background identities not recorded");
        const std::string live = runWslProbe(launch, identityScript(linuxProcesses, false));
        for (const auto& process : linuxProcesses) {
            require(live.find("NPP_ALIVE|" + process.kind) != std::string::npos &&
                live.find("NPP_UNKNOWN|" + process.kind) == std::string::npos,
                "WSL fixture did not establish live identity for " + process.kind);
        }
    }
    session.requestStop();
    require(session.waitForStop(2500), "bounded stop failed");
    const ULONGLONG deadline = ::GetTickCount64() + 5000;
    while ((!broker.stopped() || !shell.stopped()) && ::GetTickCount64() < deadline) ::Sleep(5);
    require(broker.stopped() && shell.stopped(), "owned Windows processes survived stop");
    if (id == L"wsl") {
        bool gone = false;
        const ULONGLONG linuxDeadline = ::GetTickCount64() + 10000;
        do {
            const std::string report = runWslProbe(launch, identityScript(linuxProcesses, false));
            gone = true;
            for (const auto& process : linuxProcesses) {
                gone = gone && report.find("NPP_GONE|" + process.kind) != std::string::npos &&
                    report.find("NPP_ALIVE|" + process.kind) == std::string::npos &&
                    report.find("NPP_UNKNOWN|" + process.kind) == std::string::npos;
            }
            if (!gone) ::Sleep(100);
        } while (!gone && ::GetTickCount64() < linuxDeadline);
        require(gone, "WSL attached Linux shell/foreground/background process survived stop");
        linuxGuard.verified = true;
        std::cout << "SHELL_SMOKE PASS wsl-attached-linux-shell-foreground-background-stop\n";
    }
    std::cout << "SHELL_SMOKE PASS shell=" << wideToUtf8(id)
        << " unicode-space-cwd input resize windows-process-stop\n";
}

} // namespace

void runShellSmokeTests()
{
    DirectoryFixture directory;
    std::vector<ShellInfo> cached;
    for (const std::wstring id : {L"cmd", L"powershell7", L"gitbash", L"wsl"}) {
        const auto launch = discover(id, directory.path, cached);
        cached = launch.catalog;
        testShell(id, launch, directory.path);
    }
}

} // namespace nppterminal::tests
