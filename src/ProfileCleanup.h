#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nppterminal::profilecleanup {

// The profile directory is deliberately self-describing.  The helper accepts
// only this exact directory naming scheme and an owner marker with the
// matching nonce; pre-existing or unmarked WebView folders are never scanned
// as cleanup candidates.
constexpr wchar_t kInstancePrefix[] = L"instance-v1-";
constexpr wchar_t kOwnerMarkerName[] = L"owner.bin";
constexpr wchar_t kLeaseName[] = L"lease.bin";
constexpr std::size_t kNonceBytes = 16;
constexpr std::size_t kMaxTrackedProcesses = 32;

enum class CleanupPhase : std::uint32_t {
    Active = 1,
    Closing = 2,
    Released = 3,
};

struct FileIdentity {
    std::uint64_t volumeSerial = 0;
    std::uint64_t fileIndex = 0;
};

struct ProcessIdentity {
    DWORD processId = 0;
    FILETIME creationTime{};
};

struct OwnedProfile {
    std::wstring rootPath;
    std::wstring instanceId;
    std::wstring parentIdentity;
    std::array<std::uint8_t, kNonceBytes> nonce{};
    FileIdentity rootFileId{};
    ProcessIdentity hostProcess{};
    std::vector<ProcessIdentity> browserProcesses;
    bool processSnapshotComplete = false;
    CleanupPhase phase = CleanupPhase::Active;
    HANDLE lease = nullptr;
};

bool queryProcessIdentity(DWORD processId, ProcessIdentity& identity);

// Creates a new instance-v1-CSPRNGGUID folder and takes its exclusive lease.
// The caller owns the returned lease until releaseOwnedProfileLease().
bool createOwnedProfile(const std::wstring& parentPath,
    const ProcessIdentity& hostProcess, OwnedProfile& profile, std::wstring& error);

// Updates owner.bin while the caller holds the exclusive lease.  The marker is
// fixed-size and bounded; an incomplete or mismatched marker is rejected by
// the helper rather than guessed at.
bool updateOwnedProfile(OwnedProfile& profile, CleanupPhase phase,
    const std::vector<ProcessIdentity>& browserProcesses,
    bool processSnapshotComplete, std::wstring& error);

// Re-opens the lease after the WebView2 BrowserProcessExited proof.  The
// operation is restricted to a marker already in Closing or Released phase.
bool acquireOwnedProfileLease(OwnedProfile& profile, std::wstring& error);
void releaseOwnedProfileLease(OwnedProfile& profile);

// Starts the adjacent, version-matched helper using an absolute path.  This
// only creates the process; cleanup and all bounded filesystem work run there.
bool launchCleanupHelper(const std::wstring& moduleDirectory,
    const OwnedProfile& profile, std::wstring& error);

// Starts the same adjacent helper in bounded recovery mode.  It examines
// only directly-owned, marked instance-v1 folders.  It removes Released
// folders after their BrowserProcessExited proof; Closing and ambiguous
// folders remain untouched because a PID snapshot alone is not proof that all
// WebView resources have exited.
bool launchRecoveryHelper(const std::wstring& moduleDirectory, std::wstring& error);

// Broker entrypoint hook.  It handles only:
//   NppTerminalBroker.exe --cleanup instance-v1-GUID noncehex
//   NppTerminalBroker.exe --cleanup-recover
// and returns a process exit code.  handled is false for normal broker IPC.
int runProfileCleanupMode(int argc, wchar_t** argv, bool& handled);

#ifdef NPPTERMINAL_TESTS
// Test-only diagnostic entrypoint.  It invokes the same bounded cleanup path
// as the disposable helper and returns its internal error text to the fixture.
int cleanupProfileForTest(const std::wstring& parentPath,
    const std::wstring& instanceId,
    const std::array<std::uint8_t, kNonceBytes>& nonce,
    std::wstring& error);

// Test-only fixture teardown. The caller must have independently established
// that the browser family has exited; this function never infers that proof
// from a PID snapshot. It revalidates the supplied owned profile, promotes a
// Closing marker to Released, and invokes the production bounded cleanup path.
// It accepts only the exact OwnedProfile identity and never arbitrary paths.
int cleanupOwnedFixtureForTest(OwnedProfile& profile, std::wstring& error);
#endif

} // namespace nppterminal::profilecleanup
