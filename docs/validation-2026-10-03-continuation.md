# Continuation validation, 3 October 2026

This records Phase 1 hardening under the user's request to continue the full plan. It is development evidence, not v1 release certification. The installed Notepad++ instance and its configuration were not changed.

## Environment

Windows 11 Pro 10.0.26300.9457; Notepad++ 8.9.8.1 x64; Evergreen WebView2 154.0.4258.48; MSVC v143. Integration checks use the cached disposable portable host and stock comparison. The usable launch is `NppTerminalDevHost.exe -multiInst -nosession`; adding the unsupported `-noUpdater` option caused both plugin and stock test hosts to fail at startup.

## Native and frontend checks

The primary versioned build through `scripts/build.ps1 -SkipBootstrap` emitted 0.1.3, followed by 0.1.4 after correcting a flaky blocked-input fixture. A single accepted 64 KiB input write can drain before cancellation; the corrected fixture fills the bounded input path and observes a specific unfinished write before requesting stop. The failed probe is not counted as a pass.

For 0.1.4 the primary combined command passed:

```powershell
./out/x64/Release/NppTerminal.Tests.exe --unit --conpty --stress --blocked-stop --job-probe --stream-count=10000 --stream-seconds=2
```

The 100 mixed natural-exit/Kill/Restart cycles settled at 96 handles and 4 threads, matching the baseline; worst measured explicit stop was 9 ms. The two-second producer verified 47,260 ordered complete lines / 839,661 bytes and reported 17 bytes of a deliberately truncated final record. The parent-job probe terminates only its helper while keeping the outer job open, so cleanup must come from the session's inner kill-on-close job.

Eight frontend bridge tests passed. They simulate xterm/host transport and do not certify browser rendering, IME, clipboard, or full-screen applications.

The copied 0.1.3 harness recorded a full `stream_seconds=1800` result: 14,430,140 ordered complete lines / 291,921,927 bytes, 20 final partial bytes, maximum partial buffer 38 bytes. Its streaming transport/verifier was unchanged by the later blocked-input fixture correction. The process has ended, but the tool session and final standalone PASS/exit status were not retained. This is useful long-stream evidence, not a certified passing run. A persistent validation runner is being added to capture exact exit results. This harness does not establish a WebView/xterm checksum or 30 minutes of UI stress.

Build 0.1.6 through `scripts/build.ps1 -SkipBootstrap` and the same primary combined command passed after the resize-mutex and named-profile changes. The 100 cycles matched 94 baseline/settled handles and 4 threads, with worst stop 6 ms. The two-second producer verified 57,785 ordered complete lines / 1,029,111 bytes and reported 17 final partial bytes. Production resize now snapshots ConPTY ownership under a short mutex and performs the OS resize call outside that lock; the supervisor is its sole caller and teardown owner.

## Integration and resources

The new `scripts/validate.ps1` was executed against build 0.1.6 on 3 October 2026. The initial runner attempt passed all native checks but failed to launch npm because its lookup returned multiple installation paths; that runner failure is retained. After selecting one executable path, the rerun passed frontend, unit/ConPTY, stress, blocked-stop, job-probe and 10,000-line stream checks with exact exit code 0 and native final PASS lines. Evidence: `out/validation/0.1.6-20261003T115002766Z/summary.json`. The optional thirty-minute run was not repeated against this pre-refactor build.

The initial continuation portable host was never-started at 65,257,472 private bytes with no browser or shell family. A stock small-window comparison measured 66,031,616 bytes. These separate launches contain normal measurement noise.

The initial small-window active sample measured 174,718,976 browser-family private bytes. A matched maximized-host comparison measured 171,118,592 incremental host-plus-browser bytes (163.19 MiB), excluding shell and ConPTY. The terminal grid was not the standardized 80x24, so neither sample certifies the resource budget. Both leave the provisional 150 MiB target unmet.

The official [WebView2 memory-target API](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_19) is intended for inactive views and may swap memory to disk with a performance cost. It does not establish an active private-commit reduction. No memory-target or experimental browser flag was applied to declare the active budget passing.

Abrupt termination of the owned portable host was checked against its recorded host/browser/shell/ConPTY PIDs. All were absent at the settled follow-up. This checks the actual portable host's abrupt exit; it does not establish the final `NPPN_SHUTDOWN` callback's bounded join contract.

The first profile-deletion attempt (0.1.3 runtime check) released the browser and shell after Kill, but retained `Default` and a fallback `Profile 1`. The owned data tree shrank from 9,377,632 to 7,772,009 bytes; full cleanup did not pass. Build 0.1.6 instead creates a named disposable profile, validates its returned name and exact owned parent, and requests deletion through WebView2 before controller close. Its runtime check reduced the named profile from 3,428,760 bytes to six residual files / 10,666 bytes, but the full data tree retained 7,633,790 bytes of browser caches and residual state. Browser/shell processes were absent after Kill. Profile deletion is best effort and does not complete the full inactive-folder cleanup gate. The fresh small-window active sample was 168.02 MiB over the recorded stock small-window baseline, excluding shell and ConPTY.

## Open gates

The broker/profile-cleanup refactor reserved version 0.2.0 through `scripts/build.ps1 -SkipBootstrap -VersionBump Minor`. That build failed on a version-API function-pointer type and three stale `tryPush` test calls; 0.2.0 is consumed. The helper executables compiled, but the combined build and runtime checks did not pass. These compile defects were corrected before the next official build.

Build **0.2.1** passed through `scripts/build.ps1 -SkipBootstrap` on 3 October 2026. The first persistent validation run failed: frontend, blocked-input cancellation, parent-job/abrupt-host cleanup and the 10,000-line producer passed; unit/ConPTY and stress failed the synchronous Starting notification assertion; broker stall, blocked command, orphan policy and partial thread startup failed handle-baseline assertions; the first Released-folder cleanup fixture failed. No timeouts occurred. These failures are retained at `out/validation/0.2.1-20261003T152015235Z/summary.json` and are under investigation, not waived. The thirty-minute producer and GUI integration were not run against 0.2.1.

Source review closed stale browser-exit proof reuse: only a notification delivered after Closing is persisted can authorize Released. A bounded expiry for a missing post-close WebView callback and event-ordering injection coverage remain required; the current observer can retain its COM cycle when no exit event arrives. The production helper's path-based traversal checks identity and reparse boundaries but does not constitute a formal defense against an adversarial same-user rename race.

Build **0.2.2** passed and restored unit/ConPTY and 100-cycle results: 135 baseline/settled handles, 4 threads, worst stop 32 ms. Fault fixtures initially compared against 117 handles before the first process launch; all settled at 135. Their corrected setup performs a real normal start/stop before recording a strict baseline. Build **0.2.3** passed, followed by passing frontend, unit/ConPTY, 100-cycle, blocked-input, abrupt-host/job, stalled-broker, blocked-command, three-cycle orphan retention/reclamation, partial thread-start failures and ordered output checks. Profile cleanup remained failed. Evidence: `out/validation/0.2.3-20261003T153235983Z/summary.json`. The exact cleanup error was directory reservation access denied while the contained lease was open. The subsequent source fix closes the lease after locked identity validation and before the atomic rename; it still requires post-rename identity/nonce/parent/reparse validation before deletion. That fix has not yet passed runtime checks.

A copied 0.2.2 artifact is running the persistent optional thirty-minute producer. Its final result remains pending; earlier failing short test groups are retained in that run's overall summary. Later builds cannot replace its copied binaries.

Windows 10 22H2 is unavailable here and remains not run. The broker refactor removes the retained-panel/live-supervisor fallback, but current failing validation and missing GUI callback-lifetime evidence leave final shutdown uncertified. Local Windows 11 cancellation probes do not certify the supported-OS matrix.

Not run: standardized 80x24 resource/latency measurements; full renderer/bridge streaming stress; 100 WebView lifecycle cycles; canceled host shutdown with unsaved documents; renderer-crash/missing-runtime fault matrix; protected/clean-machine installation; DPI/undocking/multiple-instance matrix; interactive IME/AltGr/clipboard/full-screen CLI acceptance. A fixture Save As dialog could not be controlled reliably by the available UI helper, so the canceled-shutdown test is not counted as completed.

Shell discovery, additional shells, directory resolution, settings, themes, clipboard/accessibility refinements, and v1 release certification remain pending the plan's development gates and the requested product direction.
