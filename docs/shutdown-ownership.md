# Shutdown ownership

Updated 4 October 2026. This is the implementation contract for the Phase 1 broker refactor, not a claim that all validation has passed.

## Process boundary

Each session has a native `NppTerminalBroker.exe` beside the DLL. It owns ConPTY and all terminal I/O, resize, process-wait and close workers. The DLL owns a noninherited kill-on-close outer job and the local IPC endpoints. It creates the helper suspended, assigns it to that job, and resumes it only after containment succeeds. The helper's shell job remains responsible for the session's descendants. A restrictive parent job that rejects this containment is an actionable startup error.

The DLL contains no terminal worker thread. Its session facade polls bounded local IPC from the panel's active-session timer. Separate command, control and output channels allow stop/error handling without waiting for the renderer. A nonblocking output bridge carries bounded backpressure. Input acknowledgements still mean that the ConPTY write completed. Output EOF fences native exit delivery; the panel then waits for queued and in-flight renderer acknowledgements before displaying Exited. A missing renderer acknowledgement produces an actionable error after five seconds rather than an indefinite finishing state.

## Final shutdown

Shutdown disables panel dispatch and callbacks, cancels input, requests the helper to stop, and waits only within the two-second budget. A stalled helper is terminated through the outer job. There is no panel retention or DLL-worker join fallback. Pending overlapped command memory must remain valid until completion. An exceptional cancellation failure can retain one raw allocation and completion event per facade, with no callback or DLL thread. Polling reclaims it once signaled; restart is refused while it remains unresolved. Final destruction may retain that one record until process exit. Deterministic policy tests cannot prove a universal Windows cancellation-completion bound.

WebView2 has a separate COM callback lifetime. The DLL is pinned lazily before its first asynchronous creation request because pending creation callbacks cannot be revoked. Closed owners suppress callbacks; an independent cleanup state waits for BrowserProcessExited before marking the whole owned data folder Released and launching the cleanup helper. A message-only STA window bounds this observer to 30 seconds; expiry detaches its event handler and COM references while preserving the Closing marker. A late environment creation callback attaches only to the independent state, never the closed panel. Recovery never promotes an ambiguous Closing folder from a PID snapshot. A host that exits before the release proof can therefore leave a marked folder requiring explicit recovery. The helper checks ownership, identity, lease and reparse boundaries, preserves metadata across partial deletion, and has a 30-second work deadline plus a 45-second process watchdog.

## Required evidence

- Existing protocol, generation, bounded-input, ordered-output and natural-final-tail checks pass through the broker.
- Stop with a stalled helper, blocked command write or full renderer output returns within the budget and leaves no DLL terminal threads or owned descendants.
- Parent-job and abrupt host termination checks keep the enclosing test job open while observing cleanup, avoiding a false pass caused by the test's own cleanup.
- One hundred mixed lifecycle cycles settle to their handle/thread baseline.
- A thirty-minute ordered producer retains its final exit code and passing verdict.
- Windows 11 x64 executes the shutdown matrix. Windows 10 is outside the supported scope following the user's 4 October decision.

The helper is an implementation binary, not a bundled shell or runtime. Its embedded version must match `VERSION`; packaging rejects a stale helper before staging.

## 4 October 2026 reservation retry

0.3.23 retries MoveFileEx reservation up to four times for ERROR_SHARING_VIOLATION or ERROR_ACCESS_DENIED, waiting 25 ms between attempts within the existing cleanup deadline. A concurrent validator can briefly keep a contained file open after the exclusive lease closes. Every successfully claimed tombstone still needs the recorded root identity and nonce before any deletion. A persistent lock or failed proof remains a conservative failure; Closing folders and unrelated profiles are not promoted or removed. The transient-reader and repeated concurrent-claim regressions pass in the current standard run.


## 4 October 2026 Ctrl+C correction

0.3.26 omits CREATE_NEW_PROCESS_GROUP when creating both the contained broker and its ConPTY shell. That flag disables Ctrl+C for the group and descendants; the inherited suppression survived removing only the shell flag. Dedicated ConPTY and kill-on-close jobs still own containment and cleanup. No GenerateConsoleCtrlEvent-based group termination is used. Native signal, lifecycle, parent-job, stalled/blocked boundary and shell checks passed in the current standard run, but concurrent profile cleanup failed with both helpers returning exit 2. The earlier bounded reservation retry is therefore not a certified resolution of that race.
