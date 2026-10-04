# Implementation status

## Current About feature, 0.5.0 — 4 October 2026

Version **0.5.0** adds a native About dialog with version, brief description, author Duc Le and a clickable website. The official Release x64 build and all 19 standard validation groups passed; the About content/menu/close smoke checks also passed. See [the dated validation record](validation-2026-10-04.md) for evidence and limits. Existing manual/resource release gates remain open.

## Historical remediation, 0.4.9 — 4 October 2026

The remediation handoff reported version **0.4.9**, the final handoff candidate. A fresh, hash-verified 109-file corresponding-source/bootstrap build passed; the input comparison is `out/validation/audit-remediation-20261004/clean-build-input-comparison-0.4.9.json`. Standard validation passed 19 of 19 groups with exit 0 in `out/validation/0.4.9-20261004T133857640Z/summary.json`, ending at `2026-10-04T13:40:24.5776569Z`. The [audit remediation record](audit-remediation-2026-10-04.md) is the finding-by-finding evidence and limits.

F-01, F-04, F-05, F-06 and F-07 have final native/frontend passes. F-03's bounded transport and panel lifecycle checks passed: the visible owned fake-host panel transferred 2,097,609 bytes in 467 ms (4.28359 MiB/s); quiet echo/ack p50 was 30/16 ms and p95 33/33 ms across 20 samples, with render p50/p95 33/35 ms; streaming child responses measured output p50/p95 15/32 ms, ack 15/18 ms and render 30/33 ms; hidden mode used two polls over 250 ms. F-04 covered 20 standard claimant pairs plus 100 focused pairs. F-06's exact 40,000-WCHAR (about 80 KiB) HKCU pre-allocation fixture passed.

F-02 remains open and deferred by the user's decision to keep WebView2 and leave the 150 MiB active-memory gate open. The settled 0.4.3 hardware samples were 165.92, 166.25 and 169.04 MiB; browser readings were 168.11–171.20 MiB, with shell 27.81–27.86 MiB and ConPTY 1.36 MiB reported separately. The final 0.4.9 reference used a matching 1300x1600 stock baseline of 71,962,624 bytes, with all three handoff baseline samples matching. Never-open visible idle dock was 6.25 MiB below stock (measurement noise); after hide, idle dock with no heavy components was +1.10 MiB with zero WebView, broker, ConPTY or shell children, supporting the ≤5 MiB never-open gate for that full-viewport match. Final active 80x24 captures at 13:45:46, 13:46:26 and 13:47:29 measured 177.93, 178.16 and 178.12 MiB, above budget; browser was 180,617,216–180,789,248 bytes (about 172.25–172.41 MiB), broker 1.96–1.99 MiB, shell about 27.08 MiB and ConPTY 1.37 MiB. The initial hidden CPU capture was confounded by foreground/painting activity. No `--disable-gpu` or InPrivate shipping change was adopted.

The panel evidence uses a visible owned fake host, the native protocol path and actual shipping xterm assets; it is not a physical keyboard or full Notepad++ operator test. The final reference was Windows 11 build 26300.9457, Notepad++ 8.9.8.1, and WebView2 154.0.4258.53. Full IME, AltGr, DPI, accessibility, clean-machine, protected-installation and related manual checks remain open. Raw evidence is under `out/validation/audit-remediation-20261004`; the final package has 27 allowlisted files, with package/source inspection and exact hashes recorded in `artifact-handoff-0.4.9.json` there.

## Historical 0.3.26 continuation — 4 October 2026

Development candidate **0.3.26** restores Ctrl+C by removing CREATE_NEW_PROCESS_GROUP from both broker and shell creation. The dedicated ConPTY and kill-on-close jobs retain session containment. A real native signal regression failed before the complete fix and passed afterward; the actual portable Notepad++ terminal then interrupted Python with KeyboardInterrupt and interrupted PowerShell Start-Sleep, returning to usable prompts.

The official incremental Release x64 build passed on 4 October 2026 using the existing generated dependencies/assets. This candidate has not received a new clean-source bootstrap build. Its standard validation ended at `2026-10-04T09:39:28.8630187Z`: **18 of 19 groups passed; concurrent profile cleanup failed** with both claimants returning exit 2. Preserve this failure; the earlier 0.3.23 passing run does not supersede it. Current-candidate memory, latency and 30-minute checks were not run. The unresolved 0.3.23 active-memory result remains 168.65–170.63 MiB versus the unchanged 150 MiB budget. Clean/protected installation and the full input/IME/AltGr/clipboard/full-screen/DPI/accessibility/original-name-host matrix remain open. This is a development package, not a certified release.

Bounded 0.3.26 native UI observations passed Python and PowerShell Ctrl+C, multilingual entry/output, panel-X hide and editor-shortcut reopen during a 120-row producer with the same ten recorded host/session/browser identities alive, scrollback navigation, natural shell exit retaining final output as read-only, and normal final host close. The host is an isolated renamed portable Notepad++ 8.9.8.1 executable with the local GDI editor setting, at the current DPI; these results do not certify the broader deployment or interactive matrix. Evidence: `out/validation/continuation-20261004/interactive-acceptance-0.3.26.md` and the dated standard summary under `out/validation/0.3.26-20261004T093756193Z`.

## Previous continuation, 0.3.23 — 4 October 2026

Current development candidate **0.3.23** passed its clean-source build and all **19 standard validation groups** on 4 October 2026, ending at `2026-10-04T08:57:57.6552850Z`. Release certification is incomplete: three settled 80x24, 5,000-line-scrollback samples measured **168.65–170.63 MiB** of incremental host + WebView + broker private commit against the unchanged 150 MiB budget. Shell and ConPTY memory are reported separately. Interactive input/IME, latency and clean/protected installation acceptance remain open. The 30-minute result belongs to historical 0.3.18; that long check was not rerun for 0.3.23.

The native dock now has a caption, DPI-scaled controls and a DPI-aware font whose ownership is checked through repeated create/destroy cycles. The renderer uses a shell-neutral running label and accessible terminal name. Its xterm 6 wrapper suppresses legacy native scrollbars while xterm retains its own scrolling control.

The first 0.3.21 standard run failed concurrent profile cleanup. Two helpers both failed to reserve the released directory. A new 0.3.22 regression reproduced a transient contained-file reader preventing reservation with Access denied. The final fix retries reservation up to four times for sharing/access failures with 25 ms between attempts and the existing overall deadline. Nonce, phase and post-claim file-identity checks still guard deletion. 0.3.23 passed the full standard matrix, ten concurrent pairs, the transient-reader regression, and five further focused runs containing fifty more concurrent pairs. Persistent locks, wrong nonces, malformed markers and reparses remain refusal/preservation cases.

Computer Use is connected again. Renaming verified portable executables gives process-backed targeting; GDI editor rendering makes the disposable host usable. Original-filename portable windows still encounter a canonical app ownership mapping problem. Current native observations include idle restored dock, editor Toggle shortcut, readable caption/toolbar, positive grid resize, Settings values/Cancel, Refresh and hide preserving the shell, Restart, Kill/Start, undock/redock, canceled shutdown and ordinary quiet-prompt shutdown. Some observations are from 0.3.20/0.3.21 and retain those exact versions; 0.3.23 rechecks final rendering, lazy startup, shortcut, grid, memory and ordinary close. These bounded renamed-host observations do not certify the full original-filename, interactive, accessibility or deployment matrix.

Evidence is in `out/validation/continuation-20261004`: `clean-cleanup-fix-build-0.3.23.json`, `profile-recheck-0.3.23.json`, `ui-observations-0.3.23.json`, `active-memory-gate-0.3.23.json` and `reference-machine-0.3.23.json`. The standard run is `out/validation/0.3.23-20261004T085634506Z/summary.json`. The prepared operator form is `clean-machine-checklist-0.3.23.md`; its execution rows remain unfilled. No resource budget was relaxed, clean-PC result invented, or release published.

## Historical 0.3.16 clean-source validation

The archived feature build was **0.3.16**, rebuilt with the official bootstrap/build script from a fresh, hash-verified corresponding-source archive outside OneDrive. All **19 validation groups passed**, including its thirty-minute native stream: 19,096,948 ordered lines and 389,924,891 bytes, overall exit 0 at `2026-10-04T02:56:29.0169604Z`. The clean snapshot contained 105 files and no build output, cached SDK, downloaded packages or node_modules. This workspace has no Git repository, so this proves a fresh source-snapshot rebuild rather than a clean Git checkout. Earlier 0.3.11/0.3.15 results remain historical evidence. Last fully validated GUI development milestone: **0.1.6**. No 0.2.x or 0.3.x build is a validated Notepad++ GUI or certified release. See [4 October evidence](validation-2026-10-04.md) for the archived results and limits.

The feature build adds contained asynchronous discovery for installed PowerShell 7, Command Prompt, Git Bash and WSL; cached selection/Refresh; directory priority and Open Terminal Here; atomic native settings; light/dark theme mapping; and bounded native Unicode clipboard shortcuts. Launch-time executable and directory filesystem checks run in the contained helper. The UI captures candidate strings without probing UNC directories. Selected-shell changes and Restart stop the old session before replacement. Refresh preserves a running session.

The current short suite adds shell-catalog/directory fixtures, settings fixtures, actual installed-shell launch/input/resize/stop checks, real shipping-xterm rendering acknowledgments and an actual-panel fixture against an owned hidden fake host. The panel fixture passed lazy creation, live Refresh, canceled/accepted Open Here with a tab-switch snapshot, canceled/accepted Kill of a queued request, fresh start and canceled host shutdown. All four installed shells passed in an owned path containing spaces and Japanese characters. The WSL fixture proved its shell, foreground program and attached background child were alive by PID/start-time identity before stop, then gone afterwards. This is a controlled attached-session result; detached Linux work and shared services remain outside the accepted cleanup promise. Real xterm parsed 306,016 bytes in 10 chunks with matching checksum and final buffer tail. These fixtures do not certify Notepad++ interactive input or IME.

URL detection F-014 is explicitly deferred under the optional security/maintenance gate. Multiple sessions, custom profiles and the other deferred scope remain outside this plan. No release gate has been waived: current Notepad++ UI/interactive acceptance, standardized resource/latency measurements, installed/protected clean-machine deployment, DPI/accessibility and multiple-host checks remain open. Earlier memory samples exceeded the provisional active budget. Native app automation remains unavailable after reboot: its tools reappeared, but the native pipe is missing even after a fresh JavaScript-kernel reset. Reconnection has not been verified.

The current broker refactor moves ConPTY workers into a contained helper process and removes the retained-panel fallback. Native containment and fault checks pass locally; GUI lifetime checks remain incomplete. WebView creation callbacks have a lazy process-lifetime module pin. The independent cleanup observer expires after 30 seconds without a post-close browser-exit event, detaches its observer and conservatively preserves the Closing folder. Policy tests cover the timer and marker transitions; live COM normal-close, expiry and initialization-time close checks pass. Windows 10 is outside the accepted support scope. The memory budget has not been relaxed; remaining feature implementation may proceed, while an unverified resource or quality check still prevents release certification.

See [3 October evidence](validation-2026-10-03-continuation.md) and [4 October evidence](validation-2026-10-04.md) for failed fixtures, measurements, limitations and not-run checks. Earlier sections below retain the initial milestone's historical evidence, including its older shutdown fallback; they are not descriptions of current broker ownership.

## Historical cmd.exe milestone

For 0.1.6, native unit/ConPTY, 100 mixed lifecycle cycles, observed blocked-input cancellation, cancellation-aware blocked-output handling, parent-job abrupt-helper cleanup, 10,000-line ordered output, and short duration-stream checks passed in the primary session. The final cycle sample matched 94 baseline/settled handles and 4 threads; worst stop was 6 ms. Eight frontend bridge tests passed. These native tests did not certify UI rendering or Windows 10.

That supervisor performed ConPTY resize outside the mutex used by UI resize/stop requests. WebView2 used a named disposable profile and disabled password autosave/general autofill. Kill released the recorded shell/ConPTY/browser family, but its profile retained six small residual files and browser caches; inactive-folder cleanup was open. Active small-window overhead was 168.02 MiB over the stock baseline, exceeding the provisional 150 MiB target. Its older long producer recorded 1,800 seconds, 14,430,140 complete lines and 291,921,927 bytes, but lacked a retained final wrapper exit status and is not a certified long-run pass. Later persistent runs supersede that evidence gap.

- Dependency/public API pins, C++17/x64 MSBuild solution, static WebView2 loader/CRT, licenses, asset build, and development packaging.
- Official bottom docking, Toggle command/Ctrl+Alt+T, lazy startup, restored idle dock, native Start/Restart/Clear/Kill/Retry controls.
- Pinned xterm page, ConPTY input/output/resize, suspended process assignment to a kill-on-close job, asynchronous supervisor and independent I/O workers.
- Bounded output/input queues, render/write acknowledgments, generation checks, message-size guards, local origin/CSP, blocked browser navigation/downloads/permissions and OSC52 writes.
- Hide preserves the running session; Kill releases it and the renderer; natural exit retains output. The accepted future WSL attached-session qualification is recorded in `compatibility.md`.

Reference environment: Windows 11 Pro 10.0.26300.9457, Notepad++ 8.9.8.1 x64, Evergreen WebView2 154.0.4258.48, MSVC 14.44.35207. GUI checks used a disposable portable host; the installed Notepad++ was not modified.

Versioning follow-up, 3 October 2026: that milestone's build was **0.1.2**. `VERSION` now controls DLL and npm metadata, normal builds increment the patch, and minor/major bumps reset lower components. Primary checks passed for multi-digit patch increments, minor/major resets, synchronized metadata, and rejection of stale DLL packaging before staging. Build 0.1.1 failed on a missing generated-header newline and its number was consumed; the corrected 0.1.2 Release build passed with matching numeric and displayed DLL versions. Packaging preserves the build version in its ZIP filename and bundled `VERSION` file. Runtime behavior tests below were performed before this metadata-only change and were not rerun for versioning.

## Executed checks

| Check | Result |
|---|---|
| Pinned bootstrap/npm installation and static asset build | Passed, including SDK SHA-256 verification |
| Release x64 native solution build | Passed |
| Native `--unit --conpty` harness, final primary-session rerun | Passed |
| Eight frontend bridge tests | Passed |
| DLL exports/imports | Six official exports present; Windows system imports only |
| Portable host load/restored dock | Passed; Start enabled, no shell/browser before explicit start |
| Start, Restart, Clear, resize and toolbar layout | Passed in portable host; Restart replaced shell/ConPTY PIDs, Clear preserved them |
| Panel X hide and shortcut reopen | Passed; same shell/ConPTY PIDs survived |
| Kill and Start again | Passed; shell, ConPTY host and attributable browser family disappeared, fresh session started |
| Normal title-bar host close with active session | Passed locally; recorded host/shell/ConPTY/browser PIDs absent at first follow-up check |
| Focused independent native review | Prior concrete defects resolved; no new material defect in the cmd.exe development scope |
| Development ZIP | Generated with DLL, static assets, dependency record, licenses and documentation |

The native harness covers protocol/base64 and binary input, generation rejection, bridge dispatch handoff/backpressure/in-flight bounds, immediate-exit final output, asynchronous input acknowledgment, resize acceptance, Kill, startup cancellation, an observed native descendant followed by natural-exit cleanup, and four starts on the same session owner. Four cycles are not the planned 100-cycle stress gate. Frontend tests use a simulated xterm/host bridge; they do not establish browser rendering, IME, clipboard or full-screen CLI compatibility.

## Historical resource evidence and unmet gates

Shortcut follow-up: the default was changed from Ctrl+backtick to Ctrl+Alt+T after the user's NppExec conflict report. The revised Release build passed, and Ctrl+Alt+T hid/reopened the dock in the disposable portable host. Notepad++ 8.9.8.1's built-in shortcut tables have no assignment for this chord; installed plugins and saved user overrides still require Shortcut Mapper conflict checks. The installed host configuration was not changed.

A final small-window sample used the same portable executable/configuration, stock plugins and UI capture workflow for the stock baseline. Samples were separate process launches and retain normal measurement noise. Private bytes were:

| State | Host | WebView family | Increment over stock, excluding shell/ConPTY |
|---|---:|---:|---:|
| Stock baseline | 66,863,104 | 0 | — |
| Plugin loaded, never started | 66,248,704 | 0 | −0.59 MiB (measurement noise) |
| Active terminal | 67,514,368 | 173,281,280 | 165.88 MiB |
| Explicitly killed | 68,943,872 | 0 | 1.98 MiB |

The active sample exceeded the provisional 150 MiB target. It was not the standardized 80×24/settling/stress matrix, so the certified budget remained unproven. Feature expansion was originally held at this gate; the user's 4 October decision permits feature implementation while retaining the release gate. Never-started overhead was within measurement noise; this single sample does not certify the provisional 5 MiB target.

Final shutdown waits up to two seconds for the supervisor. If it times out, the panel is retained and its callbacks suppressed, while the plugin stays pinned until process exit. This avoids deleting live worker owners; it **does not prove that final shutdown returns only after every worker has finished**, as the plan requires. Windows 10 ConPTY close behavior and forced-stall scenarios must be validated or ownership/OS scope revisited before release. Local fast cleanup is insufficient proof.

Not run in the initial milestone: Windows 10 22H2; protected installation/clean-machine runtime deployment; parent-job and abrupt-host termination matrix; sustained blocked-I/O shutdown; cancelled host shutdown with unsaved documents; renderer-crash/runtime-missing fault matrix; standardized resource/latency measurements; DPI/undock/multiple-instance matrix; interactive Unicode/IME/AltGr/clipboard/full-screen CLI acceptance. The archived 0.3.16 continuation later covered its 100-cycle and 30-minute native groups; those results remain historical and do not close the current 0.3.18 release gates. Inactive WebView profile-directory cleanup also remains pending.

## Original milestone remaining-work record (historical)

- P1: close shutdown, resource and compatibility gates.
- P2: host lifecycle/UX hardening.
- P3: PowerShell, Git Bash, WSL, shell discovery and directory priorities.
- P4: settings, themes, clipboard/IME, accessibility and security acceptance.
- P5: stress/matrix validation and release certification.

No commit, push, Plugins Admin submission or publication was performed.
