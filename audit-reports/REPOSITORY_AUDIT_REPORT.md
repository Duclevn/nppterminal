# Repository Audit Report

## 1. Audit Metadata

- Repository: `C:\Users\ducle\OneDrive\Apps\NppTerminal`
- Branch: `main`
- Commit: `fb030c429312677a983b647547ad7c0cbff05c4c`
- Version: **0.3.26**
- Audit date: **4 October 2026**
- Model: Codex primary session; reviewer sub-agents assisted with native transport, WebView/profile cleanup, and build/test partitions.
- Scope: whole-repository static audit, prioritizing lightweight operation and performance, followed by security, correctness, dead code, maintainability, tests, and release configuration.
- Baseline: clean working tree; 104 tracked files totaling 2,104,311 bytes. Languages are C++17, JavaScript, PowerShell, HTML/CSS, and resource/project XML. Build uses Visual Studio/MSBuild through PowerShell scripts; npm supplies frontend dependencies. Tests use a native C++ harness and Node's test runner.
- Exclusions: generated output, SDK caches, node_modules, and vendored implementation internals were excluded from ordinary code-quality criticism. Dependency configuration and shipping assets were inspected where relevant.
- Tools: read-only PowerShell commands, `rg`, Git metadata/status, Node tests, package inspector, and PowerShell parser. The karpathy-guidelines and repo-audit skills were read and applied.
- Environment limitations: no fresh native build, UI run, runtime profiling, full native validation, or independent vulnerability certification. Native observations below are explicitly identified as historical evidence.
- Report creation: user approved `audit-reports/REPOSITORY_AUDIT_REPORT.md`. This report is the only intended repository change.

## 2. Executive Summary

The code has a solid foundation, but the active terminal is not yet demonstrated to meet its lightweight and performance goals. The small shipping package and lazy browser/shell startup are strengths. The principal issues are a no-progress UI-message loop, timer-limited transport, unresolved active-memory budget failure, and a retained profile-cleanup validation failure.

There is some obsolete code, but removing it will mainly improve clarity. It will not resolve browser-family memory consumption. Avoid a broad rewrite before fixing the small scheduling defect and measuring the real resource contributors.

No confirmed exploitable security vulnerability was found in the reviewed paths. This is a scoped static assessment, not a security certification. There is no evidence requiring emergency security intervention. Release certification should remain blocked on the existing resource and lifecycle acceptance gates.

Recommended order: fix the output-message loop; measure and improve transport scheduling; resolve the active-memory budget; diagnose concurrent cleanup; then remove obsolete code.

## 3. Product Understanding

NppTerminal is a local, single-session terminal for Notepad++ x64 on Windows 11. A native bottom dock hosts WebView2 and xterm.js; a contained helper owns Windows ConPTY and the shell. Users can select discovered PowerShell 7, Command Prompt, Git Bash, or WSL, start/restart/kill sessions, use the active file's directory, and configure terminal settings.

Inputs include native commands, renderer messages, keyboard and clipboard text, settings JSON, shell-discovery registry/filesystem data, and shell output bytes. Outputs include terminal rendering, native status, settings persistence, and transient WebView profiles. No Node runtime or shell is shipped. Shells retain their own normal startup behavior and privileges.

Important workflows traced: lazy dock initialization; discovery and session start; input/output and acknowledgment flow; hide versus kill; restart and generation replacement; natural exit and output drainage; host shutdown; settings load/save; WebView restrictions and profile ownership/cleanup.

## 4. Architecture Overview

```text
Notepad++ plugin exports / notifications
    -> TerminalPanel (dock, controls, settings, session coordination)
        -> WebViewHost -> local terminal.js -> xterm.js
        -> TerminalSession -> broker IPC -> BrokerSession -> ConPTY -> shell
        -> ShellDiscoveryClient -> contained discovery helper
        -> Settings / DirectoryResolver / ShellCatalog

Shell output -> broker pipe -> TerminalSession::poll
    -> BoundedOutputBridge -> JSON/base64 -> xterm.write
    -> render acknowledgment -> bridge capacity released

WebView lifecycle -> profile marker/lease -> cleanup helper
```

The helper boundary isolates risky terminal shutdown and discovery work from the editor. Generation checks protect replacement sessions from stale messages. Queue and payload bounds make memory use more predictable.

The main concern is scheduling across the UI-owned session pump and output bridge. TerminalPanel and lifecycle modules also carry substantial state, including a few unused fields. Much of the remaining complexity has a documented containment or callback-lifetime purpose; unfamiliar safety code should not be removed simply because it is large.

## 5. Build and Test Results

Checks performed on **4 October 2026**:

| Command/check | Result | Interpretation |
|---|---|---|
| `node --test web/terminal.test.mjs` | 11 passed, 0 failed; runner reported 69.3262 ms | Frontend protocol behavior using the existing test fixture |
| `& ./scripts/inspect-package.ps1` without `-OutputPath` (JSON projected for display) | Passed: version 0.3.26, 26/26 files, 715,411-byte archive, VERSION match | Read-only inspection of existing package; not a fresh build |
| PowerShell `Parser::ParseFile` over `scripts/*.ps1` | All eight scripts parsed; zero failures | Syntax validation, not execution of their workflows |
| `git rev-parse --show-toplevel`, `git branch --show-current`, `git rev-parse HEAD`, `git status --porcelain`, `git ls-files` | Repository identity recorded; clean before report creation | Baseline and unchanged-source check |
| `rg` and `Get-Content` over implementation, tests, projects, scripts, docs, and retained validation records | Static evidence collected | No runtime performance measurement |

The retained native run `out/validation/0.3.26-20261004T093756193Z/summary.json` ended at `2026-10-04T09:39:28.8630187Z`: **18 groups passed and one failed**. Its concurrent profile-cleanup stderr records both claimants exiting 2. That run was inspected, not rerun.

A reviewer reported a lockfile-only npm audit returning zero advisories for the two production dependencies. The primary session did not independently inspect its raw tool result, so it is not used as security certification or as a release gate pass in this report.

Not run: native build, bootstrap, full validation script, packaging, fresh memory/latency/CPU measurements, native/UI regression tests, or 30-minute stream. The normal build reserves a new version, and validation generates artifacts; neither was run during the read-only audit. No version was emitted or incremented.

## 6. Findings Summary

| ID | Category | Severity | Confidence | Title | Location |
|---|---|---|---|---|---|
| F-01 | Performance/correctness | Medium | Confirmed logic; runtime impact unmeasured | Output dispatch can spin without progress | `src/Bridge.cpp:69,123` |
| F-02 | Performance | Medium | Confirmed historical record | Active-memory acceptance gate remains unresolved | `docs/implementation-status.md:7` |
| F-03 | Performance | Medium | High confidence | Timer-bound transport limits throughput and adds input delay | `src/TerminalPanel.h:33`; `src/TerminalSession.cpp:1065,1141` |
| F-04 | Reliability/testing | Medium | Confirmed retained failure | Concurrent profile cleanup fails current standard validation | `out/validation/0.3.26-20261004T093756193Z/native-profile-cleanup.stderr.log:1` |
| F-05 | Dead code | Low | Confirmed references | Obsolete bridge path and write-only panel state | `src/Bridge.cpp:46`; `src/TerminalPanel.h:144` |
| F-06 | Robustness/resource use | Low | Confirmed missing bound; impact unmeasured | Registry string allocation lacks a practical size cap | `src/ShellCatalog.cpp:147,203` |
| F-07 | Performance/cleanup | Low | Confirmed allocation | Per-input TextEncoder allocation is redundant | `web/terminal.js:136` |

Severity expresses demonstrated impact, not implementation priority. F-01 and F-02 deserve early attention because performance is the user's principal requirement.

## 7. Critical and High Findings

No finding is classified Critical or High on the available evidence. Several issues block confidence in release readiness, but this audit did not reproduce a severe outage, corruption, security breach, or measured host stall warranting stronger severity claims.

## 8. Medium and Low Findings

### F-01: Output dispatch can spin without progress

- **Category:** Performance / Correctness
- **Severity:** Medium
- **Confidence:** Confirmed control-flow defect; runtime CPU and responsiveness impact not measured
- **Location:** `src/Bridge.cpp:65` (`takeForSend`), `src/Bridge.cpp:112` (`onDispatchHandled`), `src/TerminalPanel.cpp:503` (`pumpOutput`)
- **Evidence:** Sending is refused when `inFlightBytes_ + queue_.front().data.size()` exceeds the 256 KiB limit. Reposting only checks that the queue is nonempty and `inFlightBytes_` is below that limit. These predicates differ.
- **Why it matters:** The UI may repeatedly handle and repost a message without sending output, wasting CPU and delaying other editor work until an acknowledgment arrives.
- **Trigger or precondition:** Variable-sized ConPTY output and outstanding renderer acknowledgments. One 1-byte chunk plus seven 32 KiB chunks leaves 32,767 bytes of capacity; the next 32 KiB chunk cannot fit, but reposting continues. Positive variable-length reads are normal in `src/BrokerSession.cpp:825`.
- **Recommendation:** Repost only when the queue head fits remaining capacity. Preserve acknowledgment-triggered notification so output resumes when enough capacity is released.
- **Verification:** Add a bridge regression using production-sized chunks with partial remaining capacity. Assert no repeated dispatch before sufficient acknowledgment and successful resumption afterward. Measure host CPU/input responsiveness with a slow renderer and sustained output.
- **Estimated effort:** Small
- **Change risk:** Low, provided acknowledgment wakeup is preserved
- **Status:** Confirmed issue; runtime reproduction pending

### F-02: Active-memory acceptance gate remains unresolved

- **Category:** Performance / Resource use
- **Severity:** Medium
- **Confidence:** Confirmed historical result in retained records; current-candidate footprint unverified
- **Location:** `docs/implementation-status.md:7`; `NppTerminal_Implementation_Plan.md:135`; `out/validation/continuation-20261004/candidate-state-0.3.26.json` (`UnresolvedHistoricalMemory`)
- **Evidence:** The recorded 0.3.23 incremental host + WebView + broker private commit is 168.65–170.63 MiB against a 150 MiB budget. Version 0.3.26 has no new memory/latency/long-stream result. Original sample artifacts were removed during documented cleanup; retained documentation and candidate-state metadata preserve the result.
- **Why it matters:** The recorded footprint exceeds the budget by approximately 12–14%, directly conflicting with the lightweight acceptance goal. A small ZIP does not imply a small active working set or private commit.
- **Trigger or precondition:** An active terminal at the documented settled 80x24, 5,000-line-scrollback configuration. Shell/ConPTY memory is accounted separately in the existing measurement policy.
- **Recommendation:** Measure the current candidate with separate host, browser-family, broker, and shell attribution. Optimize the largest measured contributor. If WebView's baseline prevents meeting the budget, revisit renderer tradeoffs before release certification; do not assume a rewrite is necessary before measuring.
- **Verification:** Repeat the standardized baseline and three settled samples against the unchanged budget; preserve raw samples and process identities. Include never-open, active, hidden, and killed-session states.
- **Estimated effort:** Medium for measurement; remedy depends on attribution
- **Change risk:** Low for measurement; potentially High for renderer replacement
- **Status:** Historical confirmed budget failure; current acceptance unresolved

### F-03: Timer-bound transport limits throughput and adds input delay

- **Category:** Performance
- **Severity:** Medium
- **Confidence:** High confidence from code; throughput and latency not benchmarked
- **Location:** `src/TerminalPanel.h:33`; `src/TerminalPanel.cpp:488,980,1261`; `src/TerminalSession.cpp:216,518,1065,1141`
- **Evidence:** The panel pumps session work on a 33 ms timer. Each `drainOutputPipe` reads at most 32 KiB once. Input is queued, with command writes and event acknowledgments serviced by `poll`. A live broker/running state keeps `hasPendingWork` true, including while the dock is hidden.
- **Why it matters:** Under timer-driven operation, 32 KiB / 33 ms gives an approximate 0.95 MiB/s drain ceiling before framing/rendering overhead. Input submission can wait until the next tick, and acknowledgment delivery can wait for another. Actual delay can exceed nominal intervals under load. Idle hidden sessions also retain approximately 30 scheduled polling opportunities per second.
- **Trigger or precondition:** Sustained output faster than the drain cadence, or interactive input arriving just after a tick. This is not evidence that ordinary idle CPU is high.
- **Recommendation:** Process multiple immediately available blocks within a bounded time/byte budget. Consider readiness notifications or immediate posted work for queued input. Preserve containment and UI work bounds; blindly increasing timer frequency trades delay for more wakeups.
- **Verification:** Benchmark output throughput and key-to-echo/key-to-ack p50/p95 with quiet and streaming shells; compare visible and hidden idle CPU and editor responsiveness. Use the actual panel path, not only a harness that pumps faster.
- **Estimated effort:** Medium
- **Change risk:** Medium because transport scheduling and shutdown interact
- **Status:** Confirmed scheduling characteristics; magnitude of user-visible impact requires measurement

### F-04: Concurrent profile cleanup fails current standard validation

- **Category:** Reliability / Testing
- **Severity:** Medium
- **Confidence:** Confirmed retained test failure; root cause unconfirmed
- **Location:** `out/validation/0.3.26-20261004T093756193Z/native-profile-cleanup.stderr.log:1`; corresponding `summary.json`; `docs/implementation-status.md:7`
- **Evidence:** The standard run records `native-profile-cleanup` as failed. Its log states that concurrent cleanup had no successful claimant, with both helpers returning exit 2 and neither timing out.
- **Why it matters:** Cleanup is not reliably satisfying its test contract and may leave transient profiles behind. This does not demonstrate unsafe deletion, a security breach, or a particular race root cause.
- **Trigger or precondition:** Concurrent claimants attempting profile cleanup in the existing fixture.
- **Recommendation:** Reproduce with the current release helper and inspect reservation, marker identity, file locks, and retry timing. Preserve conservative ownership checks rather than simply expanding deletion permissions or retries.
- **Verification:** Add a deterministic regression for the established cause, run the focused cleanup group repeatedly, and rerun the standard suite with concurrent claimant coverage.
- **Estimated effort:** Medium pending diagnosis
- **Change risk:** Medium; cleanup touches filesystem ownership and deletion
- **Status:** Confirmed historical failure on the current candidate; not reproduced in this audit

### F-05: Obsolete bridge path and write-only panel state

- **Category:** Dead code / Maintainability
- **Severity:** Low
- **Confidence:** Confirmed by repository-wide references and production call-path inspection
- **Location:** `src/Bridge.cpp:46`; `src/Bridge.h:52`; `src/TerminalPanel.h:144,147,152`; `src/TerminalPanel.cpp:232,255,261,616,620,639,728,997`
- **Evidence:** Production output calls `tryPush`; the blocking `push` method is called only by native tests. The condition variable supports that blocking path. `discoveryRefresh_` and `hostShutdownPending_` are assigned without being consumed; `discoveryExplicitDirectory_` is assigned/cleared without a functional read. `activeFilePath` queries `currentView` and discards it; `setStatus` ignores its `error` parameter.
- **Why it matters:** These elements suggest behavior or alternate data flows that do not exist, increasing the cost of understanding lifecycle code. Their memory/performance impact is minor; a linker may already discard unused functions.
- **Trigger or precondition:** Routine maintenance or modification of session and shutdown behavior.
- **Recommendation:** Remove write-only state and unnecessary queries after checking tests. Replace legacy blocking-bridge tests with coverage of production `tryPush` backpressure, then remove the unused blocking path and its synchronization support. Either implement a meaningful error presentation or simplify the ignored parameter separately.
- **Verification:** Repeat reference searches, run frontend/native bridge and panel tests, and compile the supported configuration. Preserve required plugin exports and externally invoked callbacks.
- **Estimated effort:** Small
- **Change risk:** Low with focused tests
- **Status:** Confirmed unused production path/write-only state; test adaptation required before removal

### F-06: Registry string allocation lacks a practical size cap

- **Category:** Robustness / Resource use
- **Severity:** Low
- **Confidence:** Confirmed missing bound; adverse runtime impact not measured
- **Location:** `src/ShellCatalog.cpp:141,147,200,203`
- **Evidence:** Git and PowerShell registry reads allocate vectors from the reported registry byte count without first imposing a practical path-string maximum.
- **Why it matters:** Oversized local installation metadata can cause disproportionate allocation or discovery failure. The helper boundary and discovery deadline reduce direct editor exposure. This is defensive hardening, not a demonstrated privilege-escalation or remote attack.
- **Trigger or precondition:** Malformed or unusually large local registry values, including user-controlled HKCU installation data.
- **Recommendation:** Reject oversized values before allocation using a documented path bound; retain type, encoding, executable, and absolute-path checks.
- **Verification:** Use a controlled registry/read-helper fixture with an oversized reported length and verify bounded rejection without launching an unexpected executable.
- **Estimated effort:** Small
- **Change risk:** Low
- **Status:** Confirmed hardening opportunity

### F-07: Per-input TextEncoder allocation is redundant

- **Category:** Performance / Cleanup
- **Severity:** Low
- **Confidence:** Confirmed
- **Location:** `web/terminal.js:40,136`
- **Evidence:** A reusable `encoder` exists, but `terminal.onData` creates another `TextEncoder` for each callback.
- **Why it matters:** This adds avoidable allocation on the input path. No meaningful memory or latency reduction has been measured; it is not a solution to F-02.
- **Trigger or precondition:** Terminal text input events.
- **Recommendation:** Reuse `encoder.encode(data).length`.
- **Verification:** Run existing frontend tests, retaining Unicode and byte-limit behavior.
- **Estimated effort:** Small
- **Change risk:** Low
- **Status:** Confirmed minor improvement

## 9. Dead, Unused, and Redundant Code Candidates

- **Confirmed unused in production:** blocking bridge `push`, with test-only callers; related condition variable belongs to that path. Remove only after adapting tests.
- **Confirmed write-only/unconsumed state:** the panel fields and discarded query listed in F-05.
- **Confirmed redundant work:** fresh encoder creation in F-07.
- **Intentional or externally referenced:** Notepad++ plugin exports, notification entry points, dialog procedures, WebView COM callbacks, and broker executable entry modes. These are not dead merely because ordinary C++ callers are absent.
- **Intentional safety complexity:** generation checks, bounded queues, process jobs, asynchronous callback lifetime management, cancellation fences, and conservative profile ownership checks have real responsibilities. No blanket removal is recommended.
- Duplicate validation at native/browser trust boundaries is not automatically redundant. It protects different callers and representations.

## 10. Security Assessment

Observed mitigations include exact local-page navigation/source checks, CSP restrictions, blocked frames/popups/downloads/permissions and remote resources, bounded renderer messages, and OSC52 clipboard suppression. See `web/index.html:5`, `src/WebViewHost.cpp:863,885,903,928,946`, and `web/terminal.js:30`.

Broker framing validates header fields and caps payloads at 128 KiB. IPC uses randomized names, local-client restrictions, first-instance creation, and explicit user/SYSTEM access. Helpers are launched suspended and placed in kill-on-close jobs before resuming. Input/output limits and session generations constrain malformed or stale traffic.

Profile cleanup checks nonces, identities, leases, and reparses, and conservatively preserves ambiguous ownership. Those safeguards should be retained while diagnosing F-04. Settings reads are size bounded and writes use an exclusive temporary file and replacement.

F-06 is a local-data robustness improvement. No confirmed exploitable security vulnerability was established. The audit did not fuzz protocols, perform adversarial filesystem races, independently certify installed Evergreen/runtime vulnerabilities, or prove every Windows API failure path. Executing the user's chosen shell is intended behavior, not itself command injection.

## 11. Performance and Resource Assessment

**Measured in retained historical records:** F-02 documents the active-memory budget miss for 0.3.23. Current 0.3.26 memory remains unmeasured. The package inspected during this audit is 715,411 bytes, approximately 699 KiB, comfortably below the stated 10 MiB package target; runtimes are excluded from that target.

**Code-derived concerns:** F-01 no-progress dispatch; F-03 polling throughput/delay; F-06 disproportionate discovery allocation; F-07 minor encoder allocation. None of their runtime CPU or latency effects was benchmarked here.

**Strengths:** browser/shell startup is lazy; output queue is bounded at 4 MiB with 256 KiB in flight; output chunks are bounded at 32 KiB; input and scrollback are limited; renderer acknowledgments follow xterm writes; discovery runs out of process; resize commands are coalesced. Hiding intentionally preserves the running session, while Kill releases the renderer.

Prioritize idle/editor responsiveness, p95 input latency, active/hidden private commit, and sustained-output behavior. Preserve raw measurements and compare the same host, runtime, grid, scrollback, shell, and settling interval. Do not interpret package size, deletion of generated files, or fewer source lines as proof of runtime efficiency.

## 12. Testing Assessment

Existing coverage is substantial for a small plugin: native protocol/bridge, ConPTY, broker containment and blocked I/O, profile cleanup, WebView security/rendering, shell discovery/smoke, settings, panel lifecycle, stress, and streaming checks. Frontend tests cover Unicode, acknowledgments, stale generations, input bounds, OSC52, clipboard messaging, and settings/theme validation.

Highest-value missing or incomplete checks:

1. F-01 partial-capacity dispatch regression using variable chunks no larger than the production 32 KiB maximum. Current capacity tests at `tests/NppTerminal.Tests.cpp:202` use 128 KiB chunks and do not cover this condition.
2. Real panel throughput and input latency under sustained output; a faster-pumping native harness does not establish the panel timer's behavior.
3. Deterministic diagnosis/regression for F-04 plus repeat concurrent claimant runs.
4. Current-version resource measurements and the documented real-host IME/AltGr/DPI/accessibility/clean-install matrix.

Eleven passing frontend tests do not supersede the failed native group or establish release readiness.

## 13. Documentation Assessment

README and implementation-status documentation honestly disclose the failed cleanup group, historical memory miss, development-candidate status, and incomplete acceptance matrix. Build/version/package behavior and security restrictions are documented. The shutdown ownership document provides useful rationale for complex lifecycle code.

Historical sections contain version-specific observations and references to removed raw artifacts. `docs/cleanup-2026-10-04.md` explicitly explains this removal. Treat historical claims as dated records, not guarantees that their artifact paths still exist. Retain raw results for future performance/reliability fixes so conclusions can be independently checked.

Project instructions refer to root npm metadata, while the actual npm manifest and lockfile are under `web/`; keep future wording aligned with the repository layout. This is a documentation clarity issue, not an observed version synchronization failure.

## 14. Build, Packaging, Dependencies, and Release Assessment

Build uses the prescribed scripts with authoritative `VERSION`. Normal native builds reserve a patch increment; packaging does not increment it and checks embedded versions. No build/version mutation occurred in this audit.

Native release configuration uses x64, static CRT, whole-program optimization, non-incremental linking, C++17, `/W4`, and SDL checks. Warnings are not fatal (`TreatWarningAsError=false`); consider a warning-clean release/CI policy, but no actual warning defect was demonstrated here. No checked-in CI workflow was identified in the tracked-file inventory.

Dependencies are pinned: WebView2 SDK 1.0.4258.31, nlohmann JSON 3.12.0, xterm 6.0.0, and addon-fit 0.11.0. Bootstrap includes SDK hash checking and lockfile-based npm installation with lifecycle scripts disabled. Pins improve reproducibility but are not proof of current vulnerability status. Installed Evergreen runtime behavior/version still requires separate acceptance evidence.

The existing binary package passed exact 26-file manifest, hash, and version checks. Test executables and Node runtime are not shipped. Licenses/notices are present; this audit is not a legal licensing opinion. The source archive was not independently revalidated during this audit; historical source-archive size/count metadata must not be confused with its current state.

Current candidate lacks a fresh clean-source build and current memory/latency/30-minute evidence. Release certification remains appropriately withheld.

## 15. Positive Findings

- Compact repository and shipping archive, with caches and generated files ignored.
- Lazy heavy-component startup protects the never-open use case.
- Purposeful separation of editor UI, browser rendering, and contained terminal workers.
- Bounded protocol messages, queues, input, and scrollback, with generation-aware acknowledgments.
- Strong local WebView restrictions and native clipboard path.
- Conservative filesystem ownership and cleanup checks.
- Atomic settings replacement and bounded configuration reads.
- Explicit shutdown ownership, cancellation handling, and useful failure-path tests.
- Exact package contents/version/hash checks and candid validation documentation.

## 16. Prioritized Remediation Roadmap

### Immediate

- Fix F-01 and add the missing partial-capacity regression. Small targeted change; preserve acknowledgment wakeups.
- Keep release certification withheld while F-02/F-04 and existing acceptance gates remain open. No emergency security fix is established by this audit.

### Short Term

- Measure F-03 on the actual panel, then improve bounded scheduling and notification behavior.
- Repeat F-02 resource measurements for the candidate being evaluated and attribute browser-family overhead before architectural changes.
- Reproduce and correct F-04 without weakening ownership checks; rerun focused and standard validation.

### Medium Term

- Harden registry allocations for F-06 and complete the documented real-host/deployment acceptance matrix.
- Preserve raw benchmark artifacts and establish repeatable performance regression checks.
- Consider a warning-clean release/CI policy after observing the existing warning baseline.

### Optional

- Remove F-05 obsolete code after adapting tests.
- Apply F-07 encoder reuse.
- Simplify confusing state and documentation incrementally; avoid unrelated module rewrites.

## 17. Items Requiring Human Verification

- Real editor responsiveness/CPU during the F-01 condition, including a slow or stalled renderer.
- Current-version memory attribution, idle/hidden CPU, input latency, and sustained throughput.
- F-04 failure cause and repeatability under local filesystem/antivirus/locking conditions.
- Installed/protected and clean-machine deployment; actual Notepad++ IME, AltGr, clipboard, full-screen apps, DPI, accessibility, and host-shutdown behavior.
- Whether measured WebView costs are acceptable for the user's lightweight requirement if small improvements cannot meet the existing gate.

## 18. Limitations and Unreviewed Areas

All major first-party subsystems were included in the static review partitions: entry points/panel/settings; broker/session/protocol/bridge/discovery/directories; WebView/profile cleanup/frontend; build/package/dependency/test/docs. This does not imply every branch was executed or every line formally proven correct.

Vendored third-party implementation internals and generated SDK/runtime internals were not audited line by line. No fresh native build, native/UI test suite, profiling session, clean-machine exercise, fuzzing, or independent full dependency vulnerability assessment was performed. Prior native results and memory records are explicitly historical; the original 0.3.23 raw memory samples are absent after documented cleanup.

No application source, test, dependency, build, packaging, or version file was modified. No commit, push, destructive Git operation, or release was performed. The report records completed inspection and checks, the retained failed validation, and work not run; it does not certify production readiness.
