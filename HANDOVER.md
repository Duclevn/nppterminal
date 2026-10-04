# NppTerminal session handover

Updated 4 October 2026. Current authoritative candidate **0.3.26**. The user authorized cleanup of obsolete generated versions and an initial commit and push to https://github.com/Duclevn/nppterminal.git. Current repository instructions govern delegation. See `docs/cleanup-2026-10-04.md` for the cleanup scope and checks.

The user explicitly authorized harmless terminal commands through Computer Use in the isolated portable acceptance instance. This overrides the skill's terminal-command restriction only for that scope. Other Computer Use guidance still applies. Computer Use is connected; no restart is indicated. Preserve unrelated user hosts, documents, settings and profiles.

Development candidate **0.3.26** restores Ctrl+C by removing CREATE_NEW_PROCESS_GROUP from both broker and shell creation. The dedicated ConPTY and kill-on-close jobs retain session containment. A real native signal regression failed before the complete fix and passed afterward; the actual portable Notepad++ terminal then interrupted Python with KeyboardInterrupt and interrupted PowerShell Start-Sleep, returning to usable prompts.

The official incremental Release x64 build passed on 4 October 2026 using the existing generated dependencies/assets. This candidate has not received a new clean-source bootstrap build. Its standard validation ended at `2026-10-04T09:39:28.8630187Z`: **18 of 19 groups passed; concurrent profile cleanup failed** with both claimants returning exit 2. Preserve this failure; the earlier 0.3.23 passing run does not supersede it. Current-candidate memory, latency and 30-minute checks were not run. The unresolved 0.3.23 active-memory result remains 168.65–170.63 MiB versus the unchanged 150 MiB budget. Clean/protected installation and the full input/IME/AltGr/clipboard/full-screen/DPI/accessibility/original-name-host matrix remain open. This is a development package, not a certified release.

Bounded 0.3.26 native UI observations passed Python and PowerShell Ctrl+C, multilingual entry/output, panel-X hide and editor-shortcut reopen during a 120-row producer with the same ten recorded host/session/browser identities alive, scrollback navigation, natural shell exit retaining final output as read-only, and normal final host close. The host is an isolated renamed portable Notepad++ 8.9.8.1 executable with the local GDI editor setting, at the current DPI; these results do not certify the broader deployment or interactive matrix. Evidence: `out/validation/continuation-20261004/interactive-acceptance-0.3.26.md` and the dated standard summary under `out/validation/0.3.26-20261004T093756193Z`.

## Current artifacts and evidence

The original package/source inspection record is `out/validation/continuation-20261004/artifact-handoff-0.3.26.json`; the post-cleanup source package inspection and hashes are recorded separately under `out`. Bounded observations are in `interactive-acceptance-0.3.26.md` and `.json`, with process identity JSONs for hide/reopen/natural exit/final close. The standard summary is `out/validation/0.3.26-20261004T093756193Z/summary.json`, overall Failed, 18 Passed and 1 Failed. Obsolete packages and raw validation artifacts were removed during the authorized cleanup; historical findings remain documented in `docs`.

The final GUI runtime bytes are the ones installed from the inspected 0.3.26 package. If documentation is repackaged after the UI observations, compare the eight shipped native/runtime members by hash and preserve the observed package proof before updating the closed fixture. Do not claim another UI run from an offline update.

## Required next work

1. Diagnose the remaining concurrent profile cleanup failure; do not dismiss it because 0.3.23 passed sixty pairs. Preserve nonce, identity, lease and reparse safety. Current failure was both helpers exit 2.
2. Resolve the active 150 MiB budget miss; current-candidate standardized measurement remains unrun. Do not disable GPU or suspend hidden running output to manufacture a pass.
3. Complete original-name host/full input, IME/AltGr/clipboard/full-screen, DPI/accessibility, multiple-host, privacy, latency and clean/protected installation checks. User accepted a clean-PC checklist, not a gate waiver.
4. After production/resource changes, final candidate needs a clean-source build and its own 30-minute stream. Historical 0.3.18 long result is not current certification.

Accepted scope: Windows 11 x64; Toggle/X hide and preserve, Kill releases; WSL attached session only, no distribution/VM shutdown; F-014 URL opening deferred. No budgets were relaxed. Historical document references to removed raw artifacts describe earlier observations and are not current filesystem guarantees.
