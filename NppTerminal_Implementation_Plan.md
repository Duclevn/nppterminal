# NppTerminal implementation plan

**Status:** implementation authorized by the user; the four-shell/settings feature set is implemented and hardening is in progress. On 4 October the user selected Windows 11-only support and authorized remaining feature implementation while preserving resource/quality checks as release gates. Actual evidence and open gates are tracked in `docs/implementation-status.md`.  
**Product:** NppTerminal, a lightweight, local, one-session terminal for 64-bit Notepad++ on Windows  
**Guiding principle:** keep the feature small, fast, optional, and understandable.
**Prepared:** 3 October 2026. Recheck dependency versions and compatibility when implementation begins.

This plan turns `NppTerminal_Requirements_Specification.md` into staged work with measurable exit gates, separating public-API facts from proposals to confirm in the compatibility phase. The original planning task created no implementation; subsequent user instructions authorize the staged work.

## 1. Scope and decisions to settle first

The MVP has one terminal session, one dockable panel, local shell processes, WebView2, xterm.js, and Windows ConPTY. It does not install or bundle PowerShell, Git Bash, WSL, a Linux distribution, Node.js, or a terminal runtime. Multiple sessions, splits, SSH, command templates, session restore, and AI-specific UI remain future work.

The proposed implementation is a native C++17 Win32 plugin from the official Notepad++ template and public APIs, using one MSBuild solution. WebView2 hosts pinned static `@xterm/xterm` and `@xterm/addon-fit` assets. Node/npm may build assets on a developer machine; the installed plugin needs neither. Use one pinned, maintained JSON library for configuration and bridge serialization, with no handwritten parser. xterm supplies emulation/rendering, ConPTY supplies transport, and the plugin has no ANSI/VT parser. Phase 0 pins the template and headers; the exact Notepad++ minimum and compiler toolset follow that audit.

The accepted baseline is Windows 11 x64, validated with the selected WebView2 distribution strategy. Windows 10 and 32-bit hosts are outside the supported scope following the user's 4 October decision; no compatibility claim is made for them.

The following product interpretation is necessary because the requirements use “closed” for both hiding a panel and ending a session:

| User action | Proposed behavior | Reason and acceptance consequence |
|---|---|---|
| Toggle or panel X | Hide the dock and preserve the running session | A fast toggle should not destroy an interactive program. The hidden renderer keeps draining output. |
| Kill | Stop the process tree, close ConPTY/I/O, destroy the WebView controller/environment, and release session state | The native panel remains available with Start. Reopening starts fresh. `confirmBeforeKill` defaults to false. |
| Natural shell exit | Keep output visible and read-only until Restart or Kill | The user can inspect the final output and receives no silent shell re-execution. |
| Notepad++ shutdown | Stop the session as part of the host shutdown contract | A bounded shutdown path is required before the plugin returns from final shutdown. |

Under this proposal, AC-013 means “no session has started or the current session has been explicitly stopped”; a hidden panel may still own a live session. Phase 0 must record acceptance of this clarification before implementing persistent hidden sessions. Killing on every hide is the alternative, but makes Toggle destructive. WSL cleanup in section 5 is a second proposed clarification; it cannot silently weaken AC-012. The original requirements file remains the source for detecting these differences.

## 2. Architecture

Keep WebView2/xterm initially because it supplies a maintained emulator and interactive rendering with little plugin code. A native alternative would need its own proven emulator integration, input handling, rendering, and accessibility work; embedding an external console window would weaken docking and lifecycle control. Evergreen saves package weight, but its active processes still cost memory. Reusing its installation does not mean sharing another plugin's controller or receiving free browser memory. P1 measures this tradeoff before release certification; feature implementation may continue under the user's 4 October decision.

The smallest useful component boundary is:

```text
Notepad++ UI thread / WebView2 STA
  PluginMain + menu/shortcut registration
  TerminalPanel + native toolbar + official docking wrapper
  WebViewHost + SessionBridge
  Settings + ThemeAdapter
  ShellCatalog + DirectoryResolver
          |
          | bounded, generation-tagged commands and output
          v
TerminalSession (UI-affine polling facade, outer kill-on-close job)
  contained NppTerminalBroker.exe per session
    ConPTY + independent output/input/command workers
    CreateProcessW child in a nested kill-on-close job
```

The Phase 1 shutdown investigation required a contained broker so ConPTY stalls cannot retain DLL terminal workers or the panel during final host shutdown. `TerminalSession` owns the outer job and bounded local IPC, while the version-matched broker owns ConPTY and its workers. See `docs/shutdown-ownership.md` for deadlines and exceptional command-buffer ownership. `ShellCatalog` and `DirectoryResolver` remain testable native logic; bounded disposable helper work keeps discovery and filesystem validation away from the UI. Panel visibility (`Visible` or `Hidden`) is tracked separately from session state, so a hidden live session cannot be confused with a stopped session.

The UI thread owns the dock window, WebView2 controller, and all WebView calls. Terminal I/O, ConPTY resize, writes, waits, and close operations run away from the UI thread. The exact minimum worker arrangement is a Phase 1 proof point: an independent output drain must remain alive while ConPTY is being closed, and input cancellation must not depend on output progress. A process wait registration may be used instead of an extra wait thread if it makes ownership clearer. No UI pointer is handed to a worker, and no thread is created from `DllMain`.

The bridge uses bounded queues in both directions. Output is coalesced into bounded chunks and sent as bytes (for example, base64 decoded to `Uint8Array` in the page) so split UTF-8 sequences are preserved until xterm receives them. The page acknowledges a chunk only after the xterm `write` callback. High and low watermarks apply to native output and in-flight page messages. Input writes are serialized; a full queue temporarily applies backpressure and disables further input until space is available. A paste has a bounded size and is rejected with a visible status message when it exceeds the limit; individual keystrokes are never silently dropped. A hidden panel continues to drain ConPTY and does not rely on `requestAnimationFrame`; shutdown can discard renderer work while the native side drains and closes. Every asynchronous action carries a session generation token, so late startup, page-ready, resize, or exit callbacks cannot affect a replacement session.

## 3. Notepad++ integration contract

The official [Notepad++ plugin manual](https://npp-user-manual.org/docs/plugins/) and [public communication contract](https://npp-user-manual.org/docs/plugin-communication/) govern host integration. P0 rechecks them and pins current headers plus the [official template](https://github.com/npp-plugins/plugintemplate). Follow its Unicode DLL exports, ABI, and `getFuncsArray` lifecycle, with short callbacks that dispatch work to the session/UI state machine. Verify actual header signatures and message return values; template code is a starting point, not permission to use obsolete or internal APIs.

The panel uses the normal docking interface with a stable, persistent dialog identifier and a lifetime that matches the host's docking expectations. The plan forbids hard-coded numeric messages, private Notepad++ symbols, global hooks, patching `config.xml`, or subclassing the editor to imitate docking. Host message constants come from the pinned public headers, and the official docking registration path is used. The plugin must not change Notepad++'s process-wide current directory, environment, DLL search path, or DPI context; child-specific values are passed through the process creation call.

The shortcut is registered through the plugin command table so Shortcut Mapper can configure it; Ctrl+Alt+T is the accepted default following the NppExec conflict report, subject to saved overrides and keyboard-layout checks. Query the current active view and file through public APIs. Store `NppTerminal.json` below `NPPM_GETPLUGINSCONFIGDIR`, with atomic write/replace and safe defaults for malformed settings. Do not guess an AppData path or ship a writable `config/` directory beside the plugin; the host query accommodates installed and portable configurations.

Theme updates use public Notepad++ theme information and the dock's normal auto-theme behavior. Avoid duplicate theme watchers/subclassing. Distinguish `NPPN_BEFORESHUTDOWN` from `NPPN_CANCELSHUTDOWN`; prepare for exit without destroying the session during a cancellable shutdown. `NPPN_SHUTDOWN` must not return while DLL workers or unsafe callbacks remain. Proposed exception to normal asynchronous operation: a short, measured join during final host exit only, while actual I/O/ConPTY close stays on workers. No nested message loop, indefinite UI wait, or detached-worker fallback is acceptable. Prove this on the oldest supported OS in P1. If safe bounded completion cannot be established, revisit helper ownership before release. See the [public notification definitions](https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h).

## 4. User flow and lifecycle

1. Loading the DLL registers commands and lightweight native state only. It does not create a shell, WebView, shell-detection thread, or probe thread.
2. Opening the command creates or shows the dock. A restored dock may be visible at Notepad++ startup, but it must not auto-launch a shell.
3. Create WebView2 on the UI apartment, with a writable per-user, per-instance user-data directory outside packaged assets and cloud-synced settings. Load the packaged page under the origin policy below. If initialization fails, show a native, actionable error and Retry without starting a shell. Offer a user-initiated link to Microsoft's runtime instructions, never a silent runtime installation.
4. After the page is ready and xterm reports a positive rows/columns grid, native code performs the first shell discovery and resolves the startup directory. Detection and filesystem validation run in the background with a timeout; the UI shows a small pending/error state rather than blocking Notepad++.
5. The selected shell is launched only after all preconditions pass. The toolbar then exposes shell selection, Restart, Clear, and Kill. Changing shell serializes stop then start; two sessions never overlap. Detection also runs on an explicit settings refresh, always in the background with a timeout.
6. Restart uses the same shell identity and recomputes the directory using the startup priority below. There is no separate restart policy in v1 and no injected `cd` text.
7. Clear calls xterm's visual clear operation and leaves the shell alive. It does not reset the shell or alter its working directory.
8. A shell exit transitions the session to read-only `Exited` after native process, ConPTY, queue, and worker resources have been released; Restart is the explicit way to start again. Renderer failure stops the current session and offers native Retry; it never silently reruns a command.

The session state machine should make invalid operations visible: `NoSession -> Starting -> Running -> Stopping -> NoSession`, with `Exited` and `Error` terminal states that require an explicit action. Visibility is an independent `Visible/Hidden` value. Start cancellation, Kill, shell replacement, panel destruction, and host shutdown all use the same cancellation path. A short bounded graceful-exit attempt may be made, but universal graceful exit is not promised; the job is terminated when the bound expires so host shutdown is safe, with UI wording that unsaved shell work may be lost. `confirmBeforeKill` defaults to false and, when enabled, applies to user destructive actions; host shutdown never waits for a prompt.

The directory priority is: explicit Open Terminal Here path, current active file directory, configured default directory, Notepad++ process **working directory**, then user home. Snapshot editor context at the user's request, before asynchronous discovery. Unsaved files and unavailable automatic candidates fall through to the next usable directory. An invalid explicit path is reported instead of silently replaced. Existing sessions do not follow editor-tab changes. Test UNC paths, especially with `cmd.exe`; unsupported paths produce an actionable message, never injected `pushd` or an undisclosed directory change. Verify WSL `--cd` capability and path semantics in P3; no speculative string replacement or shell-text `cd` is used.

Provide a `Terminal Settings…` command in the plugin menu and shell selector, opening one compact native dialog:

```json
{
  "defaultShell": "powershell7",
  "defaultDirectory": "",
  "fontFamily": "Cascadia Mono, Consolas, monospace",
  "fontSize": 13,
  "scrollback": 5000,
  "confirmBeforeKill": false
}
```

The current-file startup policy is fixed in v1; `defaultDirectory` supplies its fallback. Proposed input limits: font size 6–48 CSS pixels and scrollback 0–20,000 lines. Invalid fields use defaults; preserve a malformed file until an explicit Save. Font/theme changes apply live, shell/directory changes at next start. Include Reset to Defaults and test Save/reopen plus font fallback. Enabled confirmation covers Kill, Restart, and shell replacement, without prompting during host shutdown. Shortcuts remain in Shortcut Mapper. No profile editor or general settings framework is needed.

Resolve absolute executables from known installation/registry locations and explicit PATH directories, without an implicit current/project-directory search. Resolve `cmd.exe` from the Windows system directory. Show only detected shells and cache until settings Refresh. Prefer the configured shell, then installed PowerShell 7, then Command Prompt. If an explicitly selected shell fails, show the error without silently substituting another. Expose WSL only after the launcher and a timed installed-distro query succeed. Launch Git Bash's `bash.exe`, not Mintty; verify its interactive arguments preserve the requested directory. User startup profiles are expected. NppTerminal adds no commands/editor text automatically and inherits the normal environment without recording it.

## 5. Process, ConPTY, and security design

The process sequence follows the public [ConPTY creation guidance](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session): create the connected pipes and pseudo console, create a job with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, prepare a suspended child with safe handle inheritance, assign it to the job before resume, then start I/O. `CreateProcessW` receives an absolute executable/application name, controlled argument construction, and a validated working directory. If job creation or assignment fails, the launch is rolled back; there is no uncontained fallback. The implementation performs a capability check for the selected ConPTY APIs and reports an unsupported host through the same native error path rather than loading a partial session. Tests include a host already running inside a job, because Windows job nesting constraints affect the guarantee.

The [job object contract](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) is the basis for native-tree cleanup, verified with descendants and abrupt host exit. WSL Linux processes are outside that Windows job's guarantee. Test the attached Linux shell/foreground program and background work after stopping the launcher. Proposed scope: clean up the attached session without claiming ownership of detached Linux daemons or shared WSL services. This qualification requires an explicit product decision. If original AC-012 remains mandatory and cannot be met safely, full v1 is blocked rather than declared complete with WSL omitted. Never call `wsl --shutdown` or `wsl --terminate`, which affect shared user distributions beyond this session.

Closing ConPTY follows the [ClosePseudoConsole guidance](https://learn.microsoft.com/en-us/windows/console/closepseudoconsole): keep the independent output drain alive through close, cancel outstanding input writes, and join only after the output pipe closes or the documented close condition is satisfied. There is no forced thread termination, no join from `DllMain`, and no unloading while a callback could still enter the DLL. The bounded cleanup target is provisional and must be measured rather than asserted.

Map only packaged `web/` assets to `https://nppterminal.invalid/` with WebView2 virtual-host folder mapping and cross-origin access denied; this does not start a network server. Apply a strict CSP and validate the exact page source in both message directions. Handlers also check message type, schema, byte length, session generation, and state. Terminal data uses `PostWebMessageAsJson`, never interpolation into `ExecuteScript`; expose no arbitrary host objects. Deny other navigation, frames, network requests, popups, permissions, and downloads. Links, if enabled, require an explicit user gesture and an allowlisted `http`/`https` scheme before opening the default browser. Deny OSC52 clipboard writes; explicit Unicode paste uses xterm's paste API, respecting application bracketed-paste mode. See [WebView2 security](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security) and [local virtual-host mapping](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/working-with-local-content#virtual-host-name-mapping).

The WebView2 environment uses a dedicated writable user-data folder, as described in the [user-data-folder guidance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/user-data-folder), separate from binaries and cloud settings. An unrelated plugin's browser profile is never reused. The UI remains on the WebView2 STA and follows its [threading model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/threading-model). The package must state its Evergreen runtime assumption and test the selected [distribution model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution) on clean machines.

Diagnostic logging is opt-in, bounded, and metadata-only by default. It excludes terminal bytes, commands, credentials, environment variables, and history. Opening the terminal executes no command beyond the selected shell's own normal startup; no remote service is contacted; selected editor text is never injected without a future explicit command action.

## 6. Staged implementation and exit gates

On 4 October the user authorized feature implementation to proceed before the remaining Phase 1 measurements are complete. Resource, shutdown, security, rendering and compatibility evidence remains mandatory for release; an unverified budget is not a passing result. The supported OS matrix now covers Windows 11 only. References below to the oldest supported OS therefore mean the Windows 11 baseline; Windows 10 is not a release prerequisite.

| Phase | Deliverable | Exit gate |
|---|---|---|
| 0. Contracts and dependencies | Compatibility matrix; pinned public headers/template/WebView2/xterm assets; supported Windows and Notepad++ decisions; package/license plan; hide semantics, WSL scope, shutdown ownership | Record concrete versions and test cases. Public APIs cover integration; installed runtime dependencies are limited to Evergreen and the chosen shell. Confirm the two product clarifications in section 1. |
| 1. Narrow vertical slice | Minimal public command/docking scaffold; `cmd.exe` with WebView2 + xterm + ConPTY; bounded bridge, cancellation, generation tokens, resize, output drain, job cleanup, and resource measurements | `dir`, Unicode, resize, streaming, hide/show, Kill, panel destruction, and final host shutdown pass on the oldest supported Windows and current Windows 11. No owned native child/ConPTY remains after stop. Failed shutdown proof or resource budgets prevent release certification. |
| 2. Host lifecycle and native UX | Harden P1 docking/lifecycle; configurable shortcut, toolbar, active-view query, theme events, config directory, canceled-shutdown behavior | Dock restore does not auto-launch; shortcut works from terminal and editor; shutdown can be canceled safely; final-shutdown proof remains valid. |
| 3. Shells and directories | PowerShell 7, `cmd.exe`, Git Bash, and WSL detection; selector; restart/switch; directory priority, validation, UNC handling, and WSL capability tests | Detection fixtures and installed/missing-shell matrix pass. Shell switching has no overlap. WSL is released only if its tested cleanup promise is acceptable. |
| 4. Settings, theme, and small UX | Minimal settings with safe defaults, light/dark mapping, copy/paste, clear, optional xterm links, unobtrusive errors, README draft | Malformed settings recover safely; IME/AltGr, DPI, focus, clipboard, accessibility, and theme tests pass. F-014 is included only if the security and maintenance gate remains small; otherwise it is explicitly deferred. |
| 5. Hardening and release | Stress, security, compatibility, resource, packaging, license, installation, troubleshooting, and release checklist | All required acceptance tests pass or have a documented product decision. Package contains static assets and licenses, no shell/runtime bundles, and installs through current plugin conventions. |

Phase 1 remains the critical hardening path. Remaining single-session shells/settings/UX may now proceed in parallel with its release checks. Shell profiles, multiple sessions, command templates and a second rendering strategy remain outside v1. No calendar commitments are implied by these phases.

## 7. Resource and correctness measurements

The first measurements use a named reference PC, exact Windows/Notepad++/WebView2/plugin builds, a stock Notepad++ comparison, and a settled 80x24 terminal with 5,000-line scrollback. Targets are provisional product budgets, not proven facts:

| Measure | Proposed target and method |
|---|---|
| Never-open idle | No child process, ConPTY, WebView, worker, or polling loop; incremental private bytes at or below 5 MiB against the same host without the plugin, recorded after settling. |
| Active session | At or below 150 MiB incremental private commit for the plugin plus attributable WebView process family at the settled configuration; shell memory and a WSL VM are reported separately. Shared WebView processes are measured as a family and called out. |
| Responsiveness | Local quiet command echo p95 under 50 ms; warm prompt under 1 s and cold prompt under 3 s, with profile and cold WSL startup times reported separately. |
| Cleanup | Target completion within 2 s for an ordinary local shell; shutdown tests also record the oldest supported OS and any bounded fallback path. |
| Release size | Target package under 10 MiB excluding WebView2, shell, and OS runtimes; report exact asset/license contents. |

If active browser overhead misses its budget, measure the cause and revisit the renderer decision before release certification. The user's 4 October decision permits remaining features to be implemented while this gate is open. CPU is measured during idle, prompt use, and sustained output.

## 8. Tests and requirements traceability

Automated tests cover directory/path logic, shell-catalog fixtures, settings defaults, state transitions, message schema/generation rejection, queue limits, and command construction. A Windows harness exercises real ConPTY children and job cleanup. Manual tests cover xterm rendering and Python, Node, Git, npm, ping, and interactive coding agents.

| Requirement | Phase and evidence |
|---|---|
| REQ-F-001 dockable panel | P1/P2 bottom dock, show/hide, resize, undock/restore, and stable dialog-ID tests. |
| REQ-F-002 WebView2 + xterm rendering | P1/P4 ANSI, cursor movement, Unicode/UTF-8, selection, scroll, copy/paste, prompt, and full-screen tests. |
| REQ-F-003 ConPTY backend | P1 data-flow and resize tests; source review confirms no native ANSI/VT parser. |
| REQ-F-004 four supported shell types | P3 installed/missing matrix for PowerShell 7, cmd, Git Bash, and WSL; package inspection confirms no bundled runtime. |
| REQ-F-005 shell detection | P3 catalog fixtures, timed WSL query, and safe error-state tests. |
| REQ-F-006 selector and serialized switching | P3 selector, Restart, and stop-before-start tests with no overlapping session. |
| REQ-F-007 directory priority | P3 resolver unit tests for all five priority levels and explicit Open Here input. |
| REQ-F-008 stable existing directory | P2/P3 tab-switch test confirms an active session does not follow the editor. |
| REQ-F-009 restart | P3 same-shell restart test, recomputed directory, generation-token and cancellation checks. |
| REQ-F-010 kill and cleanup | P1/P5 process, handle, ConPTY, worker, and native-tree audits after Kill, panel destruction, and host exit. |
| REQ-F-011 clear without killing | P4 visual clear test verifies shell state and working directory remain intact. |
| REQ-F-012 resize | P1/P5 repeated resize during streaming checks rows/columns and output integrity. |
| REQ-F-013 copy/paste | P4 clipboard, bracketed paste, Ctrl+C interrupt, Ctrl+Shift+C/V, IME, AltGr, and focus tests. |
| REQ-F-014 URL detection | P4 explicit `http`/`https` link test and navigation/security checks if shipped; otherwise deferred with a release note. |
| REQ-F-015 theme integration | P4 light/dark mapping and theme-change manual review without a duplicate global watcher. |
| REQ-NF-001 idle resources | P1/P5 no child, ConPTY, WebView, worker, or polling loop before first use; private-byte baseline. |
| REQ-NF-002 closed resources | P1/P5 Kill, natural exit, hide/show, and shutdown cleanup plus handle/process inspection. |
| REQ-NF-003 shell choice does not add bundled weight | P0/P5 package file/license inspection and installed-executable launch tests. |
| REQ-NF-004 responsiveness | P1/P5 UI-thread instrumentation, p95 echo latency, sustained-output, resize, and shutdown stress. |
| REQ-SEC-001 no implicit command execution | P1/P3 load/restored-dock tests and launch review confirm no plugin-injected command beyond ordinary shell startup. |
| REQ-SEC-002 no remote terminal data | P4 network-denial and source/configuration review; all transport remains local. |
| REQ-SEC-003 no sensitive default logs | P4 log inspection confirms no terminal bytes, commands, credentials, environment, or history. |
| REQ-SEC-004 safe diagnostic logging | P4 opt-in metadata-only logging and redaction tests. |
| REQ-SEC-005 no implicit selected-text injection | P3/P4 selected-text, startup, paste, and future-action boundary tests. |

Acceptance criteria are tied to concrete checks as follows:

| AC | Check and phase |
|---|---|
| AC-001 | P1/P2 open command shows bottom dock; docking/resize/restore manual test. |
| AC-002 | P3 installed PowerShell 7 launches and accepts input. |
| AC-003 | P3 missing PowerShell fixture removes it or marks it unavailable and never selects it as an invalid default. |
| AC-004 | P3 installed Git Bash appears and launches actual bash. |
| AC-005 | P3 installed WSL passes timed availability/distro query and appears. |
| AC-006 | P3 resolver test starts in the active file directory. |
| AC-007 | P2/P3 switch-tab test confirms the running session directory is unchanged. |
| AC-008 | P1/P5 interactive Python input/output manual test. |
| AC-009 | P1/P5 streaming colored `npm run dev` or equivalent test. |
| AC-010 | P1/P5 interactive CLI and full-screen/resize manual matrix, including `codex` or `pi` when installed. |
| AC-011 | P1/P5 repeated resize during output with rows/columns and output-integrity checks. |
| AC-012 | P1/P3/P5 close/kill audit of native child tree; WSL limitation decision recorded. |
| AC-013 | P1/P2 load-without-open and Kill tests prove no process/ConPTY; hidden-panel semantics follow the Phase 0 decision. |
| AC-014 | P0/P5 package and install inspection proves shells are detected/launched from the machine and not bundled. |

The stress matrix includes rapid show/hide, Clear or Kill during output, repeated Restart, resize while streaming, renderer failure, missing WebView2, invalid directories, shell exit, canceled host shutdown, and a host already in a job. Unicode includes Vietnamese, Japanese, Chinese, emoji, and split multibyte output. DPI, IME, AltGr, focus, accessibility, themes, clipboard permissions, and clean-profile installation are release checks. Run 100 start/stop cycles and a 30-minute deterministic high-volume producer with a sequence/count checksum; `ping` alone is insufficient. Require no lost/reordered bytes in normal operation, unbounded queues, surviving owned processes, or rising handle/thread baseline after settling. Cover both host views, spaces/non-ASCII paths, multiple host instances, and installed/portable configurations. Record unavailable application tests as not run, never passed.

Establish these entry points during implementation; **none exists or was executed for this planning task**:

```powershell
npm --prefix web ci
npm --prefix web run build
msbuild NppTerminal.sln /restore /m /p:Configuration=Release /p:Platform=x64
& .\out\x64\Release\NppTerminal.Tests.exe --unit
& .\out\x64\Release\NppTerminal.Tests.exe --conpty
```

Build a small native test harness alongside the DLL, isolating session logic from the host process. Each phase reports actual commands, OS/host/dependency versions, results, and failures. Use a disposable portable Notepad++ for integration, then confirm a normal protected installation. P5 packages and tests a Release build from a clean checkout.

## 9. Packaging, documentation, and future scope

Install under `plugins/NppTerminal/`, containing `NppTerminal.dll`, `web/`, and third-party notices. Prefer the SDK's static WebView2 loader library linked into the DLL, with an installed Evergreen runtime. If static linkage is unsuitable, explicitly bundle the matching x64 `WebView2Loader.dll`; never assume a system loader DLL. Include DLL version metadata, architecture, minimum host/runtime requirements, and release checksums. P0 reviews template/bundled-asset licenses and source-distribution obligations before choosing a project license.

The package has no shell/WSL/Node/runtime bundle or writable browser profile. Required web assets mean installation needs whole-folder extraction; the host's single-DLL import is insufficient. Follow [Notepad++ installation conventions](https://npp-user-manual.org/docs/plugins/#install-plugin-manually). The README covers first use, discovery, directories, hide/Kill, runtime setup, WSL limits, privacy, and diagnostics. Define bounded cleanup of inactive instance browser-data folders after their controllers/processes release them; never delete an active profile. Plugins Admin submission/publication is separate future work; prepare compatibility metadata after installation/update/removal checks pass.

Deferred until measurements and stability justify them: multiple terminals/splits, custom shell profiles, command templates and editor variables, session restore, terminal search, file/line links, task runner, SSH, remote services, cloud synchronization, environment editor, built-in Git/file UI, and AI shortcuts. URL detection F-014 is explicitly deferred under its optional security/maintenance gate. Open Terminal Here is implemented through the public active-file query and directory resolver; current host UI acceptance remains a release check. Any expansion must preserve the one-session default and the resource budgets.

## 10. Material risks and explicit review points

The highest risks are WebView2's active memory cost, `ClosePseudoConsole` shutdown behavior on supported Windows 11, final Notepad++ shutdown timing, job nesting, and WSL's inability to guarantee termination of unrelated Linux background processes. They are Phase 1/3 release gates rather than details to defer. A renderer budget miss requires a decision before release certification; a failed shutdown proof requires changing the ownership design or supported OS matrix; and an unmet WSL attached-session promise requires narrowing or blocking WSL release rather than faking a guarantee.

P0 fixes the exact Notepad++ minimum, toolchain, dependency versions, and license/distribution details. x64, Evergreen with a static loader, and current-file startup are proposed defaults. Hide/AC-013 semantics and WSL/AC-012 scope are explicit product clarifications. Shutdown ownership and browser overhead remain empirical gates; if they fail, revise the plan rather than silently relax acceptance criteria.

## Primary references

The manual was checked through its official source repository when the published site returned an access error. These links are references, not frozen dependency versions; implementation must record its selected revisions.

- [Notepad++ plugin documentation](https://npp-user-manual.org/docs/plugins/)
- [Notepad++ plugin communication](https://npp-user-manual.org/docs/plugin-communication/)
- [Notepad++ plugin documentation source](https://github.com/notepad-plus-plus/npp-usermanual/blob/master/content/docs/plugins.md)
- [Plugin communication source](https://github.com/notepad-plus-plus/npp-usermanual/blob/master/content/docs/plugin-communication.md)
- [Public Notepad++ message headers](https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h)
- [Official plugin template](https://github.com/npp-plugins/plugintemplate)
- [Creating a pseudoconsole session](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)
- [Closing a pseudoconsole](https://learn.microsoft.com/en-us/windows/console/closepseudoconsole)
- [Windows job objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
- [WebView2 threading model](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/threading-model)
- [WebView2 security](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security)
- [WebView2 distribution](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution)
- [WebView2 user-data folders](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/user-data-folder)
- [WSL basic commands](https://learn.microsoft.com/en-us/windows/wsl/basic-commands)
- [xterm.js flow control](https://xtermjs.org/docs/guides/flowcontrol/)
- [xterm.js security](https://xtermjs.org/docs/guides/security/)
