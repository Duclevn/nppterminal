# Initial implementation compatibility record

## Current About feature, 0.5.0 — 4 October 2026

Version **0.5.0** adds a native About dialog with version, brief description, author Duc Le and a clickable website. The official Release x64 build and all 19 standard validation groups passed; the About content/menu/close smoke checks also passed. See [the dated validation record](validation-2026-10-04.md) for evidence and limits. Existing manual/resource release gates remain open.

## Historical remediation, 0.4.9 — 4 October 2026

The remediation handoff reported version **0.4.9** for the Windows 11 x64 development candidate and final handoff. A fresh, hash-verified 109-file corresponding-source/bootstrap build passed. Standard validation passed all 19 groups with exit 0 in `out/validation/0.4.9-20261004T133857640Z/summary.json`, ending at `2026-10-04T13:40:24.5776569Z`. The [audit remediation record](audit-remediation-2026-10-04.md) contains the finding-by-finding evidence and limits.

F-01, F-04, F-05, F-06 and F-07 have final native/frontend passes. F-03's visible owned fake-host panel lifecycle passed using the native protocol path and shipping xterm assets: 2,097,609 bytes in 467 ms (4.28359 MiB/s), quiet echo/ack p50 30/16 ms and p95 33/33 ms, render p50/p95 33/35 ms, streaming output p50/p95 15/32 ms, ack 15/18 ms, render 30/33 ms, and hidden mode two polls over 250 ms. These are protocol and rendering checks, not physical keyboard or full Notepad++ operator tests.

F-02 remains open by the user's choice to keep WebView2 and leave the 150 MiB active-memory gate open. The settled 0.4.3 hardware samples were 165.92, 166.25 and 169.04 MiB; browser readings were 168.11–171.20 MiB, with shell and ConPTY reported separately. The final 0.4.9 reference used Windows 11 build 26300.9457, Notepad++ 8.9.8.1, WebView2 154.0.4258.53 and a matching 1300x1600 stock baseline of 71,962,624 bytes. Never-open visible idle dock was 6.25 MiB below stock (measurement noise); after hide, idle dock with no heavy components was +1.10 MiB with zero WebView, broker, ConPTY or shell children, supporting the ≤5 MiB never-open gate for that full-viewport match. Final active 80x24 captures at 13:45:46, 13:46:26 and 13:47:29 measured 177.93, 178.16 and 178.12 MiB, above budget; browser was 180,617,216–180,789,248 bytes (about 172.25–172.41 MiB), broker 1.96–1.99 MiB, shell about 27.08 MiB and ConPTY 1.37 MiB. No `--disable-gpu` or InPrivate shipping change was adopted. Full IME, AltGr, DPI, accessibility, clean-machine, protected-installation and related manual checks remain open. Raw evidence is under `out/validation/audit-remediation-20261004`.

## Historical 0.3.26 continuation — 4 October 2026

Development candidate **0.3.26** restores Ctrl+C by removing CREATE_NEW_PROCESS_GROUP from both broker and shell creation. The dedicated ConPTY and kill-on-close jobs retain session containment. A real native signal regression failed before the complete fix and passed afterward; the actual portable Notepad++ terminal then interrupted Python with KeyboardInterrupt and interrupted PowerShell Start-Sleep, returning to usable prompts.

The official incremental Release x64 build passed on 4 October 2026 using the existing generated dependencies/assets. This candidate has not received a new clean-source bootstrap build. Its standard validation ended at `2026-10-04T09:39:28.8630187Z`: **18 of 19 groups passed; concurrent profile cleanup failed** with both claimants returning exit 2. Preserve this failure; the earlier 0.3.23 passing run does not supersede it. Current-candidate memory, latency and 30-minute checks were not run. The unresolved 0.3.23 active-memory result remains 168.65–170.63 MiB versus the unchanged 150 MiB budget. Clean/protected installation and the full input/IME/AltGr/clipboard/full-screen/DPI/accessibility/original-name-host matrix remain open. This is a development package, not a certified release.

Bounded 0.3.26 native UI observations passed Python and PowerShell Ctrl+C, multilingual entry/output, panel-X hide and editor-shortcut reopen during a 120-row producer with the same ten recorded host/session/browser identities alive, scrollback navigation, natural shell exit retaining final output as read-only, and normal final host close. The host is an isolated renamed portable Notepad++ 8.9.8.1 executable with the local GDI editor setting, at the current DPI; these results do not certify the broader deployment or interactive matrix. Evidence: `out/validation/continuation-20261004/interactive-acceptance-0.3.26.md` and the dated standard summary under `out/validation/0.3.26-20261004T093756193Z`.

Updated 4 October 2026. Windows 11 x64 is the accepted scope. Current development candidate **0.3.23** passed its clean-source build and all **19 standard validation groups** on 4 October 2026, ending at `2026-10-04T08:57:57.6552850Z`. Release certification is incomplete: three settled 80x24, 5,000-line-scrollback samples measured **168.65–170.63 MiB** of incremental host + WebView + broker private commit against the unchanged 150 MiB budget. Shell and ConPTY memory are reported separately. Interactive input/IME, latency and clean/protected installation acceptance remain open. The 30-minute result belongs to historical 0.3.18; that long check was not rerun for 0.3.23.

- Target: Windows 11 x64. On 4 October the user explicitly selected Windows 11-only support. Windows 10 is outside the supported release scope.
- Initial host baseline: Notepad++ 8.9.8.1 x64, matching the installed test host and pinned public messages.
- Toolchain: Visual Studio 2022 v143, C++17, Windows SDK. Local compiler: MSVC 14.44.35207.
- WebView2 SDK: 1.0.4258.31, static x64 loader; installed Evergreen required. Initial GUI baseline runtime: 154.0.4258.48. The 4 October continuation registry observation records installed runtime 154.0.4258.53; it is not a per-test loaded-browser version capture (`out/validation/continuation-20261004/runtime-observation-0.3.18.json`).
- Static assets: @xterm/xterm 6.0.0, @xterm/addon-fit 0.11.0, npm integrity lock in `web/package-lock.json`.
- JSON: nlohmann/json 3.12.0, vendored header.
- Template/docking revision: `27b7077ba89766b3a5a136a3d71af3a0cccc7d2a`.
- Dependency hashes: `dependencies.lock.json`.

User decisions recorded during implementation:

1. Toggle and panel X hide the panel and preserve its running session. Explicit Kill releases the session. AC-013 applies to never-started or explicitly stopped sessions.
2. WSL cleanup promises the attached session (shell and foreground program). Detached Linux work and shared services are outside that guarantee. The 4 October native fixture passed for its identified shell, foreground program and ordinary attached background child; this does not extend the promise to detached work.
3. Remaining features may be implemented while resource and quality checks remain open; those checks stay mandatory release gates. This does not certify an unmeasured budget or unavailable manual test.

Installed runtime dependencies are Windows, Notepad++, Evergreen WebView2, and the selected installed shell. npm is a development-only asset build tool. DLL exports, docking registration, final shutdown, and WebView calls must follow the public host and STA contracts. No host configuration patching, process-wide environment/directory/DPI changes, or remote terminal transport is permitted.

The current WebView policy requires the runtime capability exposed by `ICoreWebView2_4` before the page is navigated. That interface supplies the frame-navigation and download denial handlers; if it is unavailable, startup fails closed with an actionable Evergreen WebView2 upgrade/retry message. The required native `--webview-security` fixture tests this absence through a test-only capability seam, then exercises the available-capability path with restrictive CSP plus navigation, popup, network, frame, permission and download probes. Network and frame denial are recorded separately as CSP and/or handler evidence; virtual-host-mapped local assets are not used as proof that `WebResourceRequested` fired.

The pinned docking contract requires `tTbData.dlgID` to be the plugin menu command ID assigned by the host. The native dialog resource ID is a separate stable identifier; it must not replace the command ID in the docking registration.

After WebView initialization, the native module remains pinned until host process exit to keep late COM callback code valid. Callback owner guards prevent access to destroyed panels. Updating/removing the DLL requires closing Notepad++; hot unload is unsupported.

Phase gates and actual evidence are tracked separately in `docs/implementation-status.md` and the [audit remediation record](audit-remediation-2026-10-04.md).

## Previous continuation, 0.3.23 — 4 October 2026

Current development candidate **0.3.23** passed its clean-source build and all **19 standard validation groups** on 4 October 2026, ending at `2026-10-04T08:57:57.6552850Z`. Release certification is incomplete: three settled 80x24, 5,000-line-scrollback samples measured **168.65–170.63 MiB** of incremental host + WebView + broker private commit against the unchanged 150 MiB budget. Shell and ConPTY memory are reported separately. Interactive input/IME, latency and clean/protected installation acceptance remain open. The 30-minute result belongs to historical 0.3.18; that long check was not rerun for 0.3.23.

The native dock now has a caption, DPI-scaled controls and a DPI-aware font whose ownership is checked through repeated create/destroy cycles. The renderer uses a shell-neutral running label and accessible terminal name. Its xterm 6 wrapper suppresses legacy native scrollbars while xterm retains its own scrolling control.

The first 0.3.21 standard run failed concurrent profile cleanup. Two helpers both failed to reserve the released directory. A new 0.3.22 regression reproduced a transient contained-file reader preventing reservation with Access denied. The final fix retries reservation up to four times for sharing/access failures with 25 ms between attempts and the existing overall deadline. Nonce, phase and post-claim file-identity checks still guard deletion. 0.3.23 passed the full standard matrix, ten concurrent pairs, the transient-reader regression, and five further focused runs containing fifty more concurrent pairs. Persistent locks, wrong nonces, malformed markers and reparses remain refusal/preservation cases.

Computer Use is connected again. Renaming verified portable executables gives process-backed targeting; GDI editor rendering makes the disposable host usable. Original-filename portable windows still encounter a canonical app ownership mapping problem. Current native observations include idle restored dock, editor Toggle shortcut, readable caption/toolbar, positive grid resize, Settings values/Cancel, Refresh and hide preserving the shell, Restart, Kill/Start, undock/redock, canceled shutdown and ordinary quiet-prompt shutdown. Some observations are from 0.3.20/0.3.21 and retain those exact versions; 0.3.23 rechecks final rendering, lazy startup, shortcut, grid, memory and ordinary close. These bounded renamed-host observations do not certify the full original-filename, interactive, accessibility or deployment matrix.

Evidence is in `out/validation/continuation-20261004`: `clean-cleanup-fix-build-0.3.23.json`, `profile-recheck-0.3.23.json`, `ui-observations-0.3.23.json`, `active-memory-gate-0.3.23.json` and `reference-machine-0.3.23.json`. The standard run is `out/validation/0.3.23-20261004T085634506Z/summary.json`. The prepared operator form is `clean-machine-checklist-0.3.23.md`; its execution rows remain unfilled. No resource budget was relaxed, clean-PC result invented, or release published.

## Installed-shell checks, 4 October 2026

Build 0.3.11 passed native launch, controlled input, resize and stop checks for installed Command Prompt, PowerShell 7, Git Bash and WSL. Each launched in the same owned directory containing spaces and Japanese characters, with the resulting Windows directory checked through that shell. The WSL test additionally required its three recorded Linux process identities to be alive before stop and gone afterwards; an unreadable identity is a failure, not evidence of exit. These are native harness results, not the full interactive application or Notepad++ UI matrix. Missing-shell selection and fallback behavior are covered by catalog fixtures.

## Historical 0.3.18 continuation evidence, 4 October 2026

Official full-bootstrap clean-source builds completed for 0.3.17 and 0.3.18 from the current 107-file source snapshot. The 0.3.17 focused security result failed on an invalid mapped-local-asset event assumption; the corrected 0.3.18 result at `out/validation/continuation-20261004/security-probe-0.3.18/result.json` exits 0 and records `network=CSP`, `frame=CSP+handler`, permission denial, download cancellation and one observed resource event. This focused native result does not certify the Notepad++ UI or an actual older-runtime installation.

All **20 groups** in the official 0.3.18 `scripts/validate.ps1 -IncludeLongStream` run passed, overall exit 0, ending at `2026-10-04T06:30:01.4524396Z`. The 30-minute stream ran for 1800.224 seconds and verified **20,128,237 ordered lines and 411,581,960 bytes**, final partial 16 bytes, maximum partial 38. Stress returned to 146 handles and 4 threads, worst stop 32 ms. The 52 retained run files were copied from the clean root with SHA-256 verification into `out/validation/0.3.18-20261004T055848333Z`; paths inside its `summary.json` intentionally retain the original clean-root locations. The runner and stream process have finished; session 61183 exited 0. The root runner log is `out/validation/continuation-20261004/validate-clean-0.3.18.log`. These harness results do not certify actual Notepad++ interaction, standardized memory/latency or installation.

At that earlier checkpoint, native Windows app automation was unavailable. The 0.3.23 continuation above supersedes that connection state and records bounded native UI observations and a failing standardized active-memory sample. IME/AltGr, interactive clipboard, DPI transitions, full accessibility, protected installation, multiple hosts and latency remain open. Windows 10 is outside the accepted scope.
