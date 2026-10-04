# Windows 11 acceptance checks

Use a disposable portable Notepad++ x64 with the entire development plugin folder installed. Record OS build, Notepad++ version, WebView2 version, package hash, and exact result for each check. Mark unavailable checks **not run**.

## Lazy initialization and docking

1. Start the host without opening the terminal. Confirm no plugin-created shell, WebView processes, or session workers exist. Record private bytes and handles against a matching stock host.
2. Open Toggle Terminal. Confirm the panel docks at the bottom, the prompt appears, and editor interaction remains responsive.
3. Resize and undock/redock. Confirm a positive terminal grid fits without corrupted output. Test DPI changes and a restored dock on a fresh host launch; restoring visibility must not launch a shell.
4. Hide with Toggle and panel X. The same shell must survive and continue draining output. Reopen it and inspect continuity.

## Shortcut and conflicts

1. Press **Ctrl+Alt+T** and confirm it opens or hides the NppTerminal panel.
2. In **Settings → Shortcut Mapper → Plugin commands**, confirm **NppTerminal → Toggle Terminal** shows **Ctrl+Alt+T** when no saved override or conflicting command is present.
3. If another plugin owns the shortcut, use **Modify** or **Clear** on the conflicting command, then assign **Ctrl+Alt+T** to NppTerminal. Reopen Shortcut Mapper and confirm the final assignment.

## About dialog

1. Open **Plugins → NppTerminal → About...** before opening the terminal. Confirm the modal dialog shows **NppTerminal**, the version from the current build, the embedded-terminal description, **Author: Duc Le**, and `https://ducle.uk`.
2. Click the website link and confirm it opens `https://ducle.uk` in the default browser. Close the dialog with **Close**, then reopen it and dismiss it with **Esc**; opening About must not create or start a terminal session.

## Input and rendering

1. Run `dir`, then an interactive Python session when installed. Verify input/output, Enter, arrows, backspace, and Ctrl+C.
2. Print Vietnamese, Japanese, Chinese, and emoji. Verify no broken multibyte sequences, including streaming output split across reads.
3. Run a deterministic numbered producer while resizing, hiding/showing, and clearing. Verify output order and count. `ping` alone is insufficient for the sustained-output gate.
4. Verify selection, focus returning to the editor, IME, AltGr, keyboard layouts, and full-screen CLI behavior.

## Shells, directories and settings

1. Test installed and absent PowerShell 7, Command Prompt, Git Bash and WSL. Refresh while a shell is running must preserve its process and generation, accept further input and resize, and update only the catalog. Explicit selection of an unavailable shell must report an error.
2. Start with an active saved file, then switch editor tabs and views. The running directory must stay fixed. Restart uses the directory snapshot taken for that action. Test configured fallback, host working directory, home, invalid paths, spaces and non-ASCII characters. Command Prompt skips automatic UNC paths and reports an explicit unsupported UNC request.
3. Open Terminal Here while running: cancel its confirmation and verify the original session survives. Accept at directory B, immediately switch back to an editor file in directory A, and verify the replacement starts in B after the original stops. Queue Open Here, then Kill, then Toggle: the canceled explicit request must not return.
4. Switch shells and Restart repeatedly. Verify the old broker/shell exits before replacement starts, the selector shows the running shell correctly, and no stale output crosses generations.
5. Save and reload every setting; Cancel must preserve disk contents. Reset changes the form until Save. Test malformed JSON, invalid individual fields, unwritable configuration and long/invalid font names. Font size is 6–48; scrollback is 0–20000. Live font/scrollback changes apply without replacing the shell; default shell/directory preferences apply at the next start.
6. Test light/dark themes and a host theme change. Test Ctrl+Shift+C/V with Unicode selection/paste and the 64 KiB limit, bracketed paste in an enabled shell, and Ctrl+C interrupt. OSC52 writes must remain denied. Verify toolbar keyboard navigation, focus and accessibility.
7. WSL: verify attached-shell and foreground-program cleanup with recorded Linux PID/start-time identities. Detached work and shared services are outside the accepted promise; never use distribution/VM shutdown as evidence of session cleanup.

URL detection F-014 is explicitly deferred under the plan's optional gate. No URL-opening feature is shipped.

## Native lifecycle and streaming harness

Run the Release test executable from a copied artifact so a later build cannot replace the binary during a long run:

```powershell
$artifact = '.\.cache\test-artifact'
New-Item -ItemType Directory -Force $artifact | Out-Null
Copy-Item '.\out\x64\Release\NppTerminal.Tests.exe' "$artifact\NppTerminal.Tests.exe" -Force
Copy-Item '.\out\x64\Release\NppTerminalBroker.exe' "$artifact\NppTerminalBroker.exe" -Force
Copy-Item '.\out\x64\Release\NppTerminalBroker.Tests.exe' "$artifact\NppTerminalBroker.Tests.exe" -Force
New-Item -ItemType Directory -Force "$artifact\web\vendor" | Out-Null
foreach ($name in @('index.html','terminal.js','terminal.css')) {
    Copy-Item -LiteralPath ".\web\$name" -Destination "$artifact\web" -Force
}
foreach ($name in @('xterm.js','xterm.css','addon-fit.js','xterm.LICENSE','addon-fit.LICENSE')) {
    Copy-Item -LiteralPath ".\web\vendor\$name" -Destination "$artifact\web\vendor" -Force
}
$testExe = (Resolve-Path "$artifact\NppTerminal.Tests.exe").Path
```

Record the command, dated result, and any reported metrics for each run:

```powershell
& $testExe --unit --conpty
& $testExe --stress
& $testExe --blocked-stop
& $testExe --job-probe
& $testExe --broker-stall --blocked-command-stop --orphan-command --thread-start-failures
& $testExe --profile-cleanup
& $testExe --webview-cleanup
& $testExe --webview-integration
& $testExe --webview-security
& $testExe --webview-render
& $testExe --shell-catalog
& $testExe --settings
& $testExe --shell-smoke
& $testExe --terminal-panel
& $testExe --stream-count=10000
& $testExe --stream-seconds=1800
```

`--unit --conpty` covers the normal native suite. `--stress` runs 100 mixed natural-exit, Kill, and Restart cycles and checks settled handle/thread baselines. `--blocked-stop` fills the input path until an observed synchronous `WriteFile` remains active, then checks cancellation; its output case uses a cancellation-aware blocked callback, so it does not prove behavior with an unresponsive operating system or renderer. `--job-probe` keeps the parent kill-on-close job open while abruptly terminating a helper, then checks that the session shell and descendant are gone; an unavailable nested-job capability is reported as **not run**. `--stream-count=10000` checks ordered, complete producer records. `--stream-seconds=1800` runs the full 30-minute producer; its deliberate final stop may leave a partial final record, which is reported with the line and byte counters. These modes do not certify WebView/xterm rendering, the bounded bridge, or a renderer-side checksum.

For a dated, persistent run that records each exact exit status, use `.\scripts\validate.ps1` or `.\scripts\validate.ps1 -IncludeLongStream`. It checks the DLL and both broker versions against `VERSION`, copies native executables into `out\validation\<version>-<UTC timestamp>`, bounds each child process, and writes `summary.json` plus stdout/stderr logs after every test. An interrupted run preserves the started test record and owned PID for diagnosis. The script does not build, exercise the Notepad++ UI, certify Windows 10 compatibility, or replace the manual renderer, shutdown, memory, and clean-machine checks above.

The runner also copies only the shipping `web/` assets beside the native executable and runs `--webview-integration`. This STA fixture creates a real hidden WebView, loads the packaged page, then exercises normal close, a suppressed browser-exit completion with observer expiry, and close during environment initialization. It checks actual event registration/removal and cleanup-state lifetime. Missing runtime/assets are required-check failures, not passing substitutes. `--webview-cleanup` separately covers marker policy and the message-only timer without a live COM environment. `--webview-render` uses shipping xterm, checks ordered write-callback byte/checksum acknowledgments and the parsed final buffer tail, including stale-generation rejection. It does not use ConPTY or certify interactive Notepad++ input/IME. Profile cleanup after close observes the original instance path disappearing; this does not independently certify all helper tombstones or sidecars.

`--webview-security` is a required native check for the current candidate. It first uses a test-only seam to remove the queried `ICoreWebView2_4` capability before production navigation; startup must fail closed with an actionable message that the runtime is too old and should be updated to Evergreen WebView2 before retrying. With the capability present, a real hidden WebView verifies the restrictive page CSP and attempts top-level off-origin navigation, a popup, an off-origin fetch, an iframe, a notification permission request and a blob download. Navigation, popup, permission and download handlers must deny their requests. Network and frame denial are accepted only with independent CSP and/or corresponding handler evidence; resources served through the mapped local page are not used as `WebResourceRequested` proof because that event is not guaranteed for virtual-host mapping. The focused 0.3.18 result is recorded at `out/validation/continuation-20261004/security-probe-0.3.18/result.json` and reports `navigation=1`, `popup=2`, `network=CSP`, `frame=CSP+handler`, `permissions=denied`, `download=1` and one observed web-resource event. This is an automated native fixture, not a Notepad++ UI test or a real old-runtime compatibility run; an unavailable runtime is **not run**.

`--shell-catalog` covers directory priority, explicit errors, fallback and executable qualification. `--settings` covers parsing, field validation, defaults and atomic persistence. `--shell-smoke` exercises actual installed shells through the contained discovery/session helpers, controlled directory-query input, resize and Windows process stop. Its WSL fixture records Linux PID plus `/proc` start time for its own attached shell and children, requires all identities alive before stop, and fails if any remain alive or unknown afterwards. Any failure cleanup targets only those exact recorded identities. Unavailable shells are reported explicitly; an absent required capability must be reflected in the release matrix.

`--terminal-panel` runs the actual panel against an owned visible fake host window, with real WebView, discovery and session helpers. It checks lazy creation, Refresh preserving a session, accepted/canceled Open Terminal Here, canceled/accepted Kill of a queued request and canceled host shutdown. Its controlled directory-query input enters the native protocol handler; this is not an end-user keyboard/Notepad++ UI test. The 0.4.9 panel benchmark transferred 2,097,609 bytes in 467 ms (4.28359 MiB/s). Across 20 quiet samples, echo/ack p50 was 30/16 ms and p95 33/33 ms, with render p50/p95 33/35 ms. Across 20 streaming child responses, output p50/p95 was 15/32 ms, ack 15/18 ms and render 30/33 ms; hidden mode used two polls over 250 ms. Quiet under 50 ms was asserted and streaming p95 was measured; these checks do not replace physical keyboard, full Notepad++ operator, or manual release gates.

The broker fault tests cover forced helper termination, a pending command write, all three partial worker-start positions, and retained-write reclamation/restart refusal. The orphan fixture delays observation of a real cancellation completion; it verifies ownership policy rather than a universal kernel timing guarantee. The blocked-input fixture replaces the test broker's input transport with an undrained 4 KiB anonymous pipe while keeping a real ConPTY and shell for cleanup. It observes an actual unfinished synchronous Windows write before stop; it does not establish that ordinary ConPTY input will naturally stall on every supported OS. Profile cleanup fixtures run the production cleanup algorithm in the test helper under an isolated, validated GUID parent to test release proof, nonce/lease rejection, malformed markers and reparse refusal. The test-only parent argument is absent from the production helper. Recovery tests require a clean baseline inside that owned parent and report unavailable capabilities as not run; preexisting production Closing folders are preserved.

## Lifecycle and errors

1. Kill an active shell and its native descendants. Confirm none remain, ConPTY handles/workers settle, and the WebView controller is released. Start again.
2. Restart repeatedly; ensure old and new sessions never overlap. Natural `exit` must leave output visible/read-only and must not silently relaunch.
3. Close the host with a running shell and descendants. Confirm no owned process survives. Repeat with sustained output and a blocked input write.
4. Initiate host shutdown with an unsaved document, then cancel it. The terminal must remain usable. Finish shutdown and verify cleanup.
5. Test missing WebView2, missing assets, renderer failure, and process/job creation failures. Errors must remain native and actionable; the host must remain usable.
6. Repeat inside a parent Windows job, and test abrupt host termination: kill-on-close must remove owned native descendants.

## Compatibility and resource release gates

- Supported Windows 11 x64 baseline; installed and portable host; two host views and multiple instances. Windows 10 is outside the accepted support scope.
- 100 start/stop cycles with settled handle/thread counts and no rising baseline.
- 30-minute deterministic streaming stress with a sequence/count checksum.
- Never-open overhead target ≤5 MiB; active plugin plus attributable WebView family private commit target ≤150 MiB, shell memory reported separately.
- Quiet echo p95 target <50 ms; warm prompt <1 s, cold <3 s; ordinary native shell cleanup target ≤2 s.
- Release ZIP target <10 MiB; inspect licenses/assets and confirm no bundled shell or Node runtime.

After `scripts/package.ps1`, run `scripts/inspect-package.ps1 -OutputPath <existing-validation-directory>\package-inspection.json` to check the exact current 27-file manifest and source/binary hashes. Only a completed zero-exit inspection certifies that check; packaging alone does not certify installation or release readiness. Dated inspections from packages created before the audit-remediation report retain the historical 26-file count.

Capture settled memory with `scripts/measure-resources.ps1 -HostProcessId <id> -BaselinePrivateBytes <stock-host-private-bytes>`. It reports host, WebView and broker memory separately from shell and ConPTY processes and collects metadata only. Use `IncrementalPluginFamilyMiB`, which includes the broker, for the current active-session budget. Match host window size, terminal grid, scrollback, stock plugins, and settling time before comparing samples. A single development sample is not a certified resource budget.

These targets are provisional until measured. Under the user's 4 October decision, feature implementation may continue while these checks remain open; failures still block release certification.

## 0.3.23 regression coverage

The standard panel fixture also checks GDI object counts across repeated idle create/destroy cycles. Profile cleanup covers ten concurrent helper pairs and a transient contained-file reader that releases during reservation. These checks exercise real ownership and filesystem behavior; they do not replace native host interaction, operator input, clean-machine or resource acceptance.


## 4 October 2026 interactive interrupt follow-up

0.3.26 adds a required Ctrl+C console-signal regression to --conpty and --stress. The child receives literal ETX through the real broker/ConPTY input path and must observe CTRL_C_EVENT without explicitly resetting inherited Ctrl+C suppression; marker observation and natural process cleanup are required. Removing the process-group flag only from the shell was insufficient because the broker flag also suppresses inherited signals. Both flags are removed; job containment is unchanged.

Actual isolated portable Notepad++ UI probes also observed Python KeyboardInterrupt and PowerShell prompt recovery, Unicode output, hidden producer completion with preserved process identities, working scrollback, and read-only final output after natural exit. These bounded observations do not fill the whole operator form. The current standard run failed concurrent profile cleanup and is not an overall pass.
