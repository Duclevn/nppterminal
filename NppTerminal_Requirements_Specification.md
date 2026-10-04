# NppTerminal — Requirements & Technical Specification

**Project:** NppTerminal  
**Target host:** Notepad++ on Windows  
**Document type:** Product Requirements + Technical Specification  
**Primary implementation agent:** Codex  
**Status:** Draft v1.0  
**Goal:** Build a lightweight, dockable, real interactive terminal for Notepad++ without turning Notepad++ into a heavy IDE.

---

## 1. Product Vision

NppTerminal is a lightweight terminal plugin for Notepad++.

It should provide a terminal experience similar to the integrated terminal in VS Code, but remain consistent with the lightweight philosophy of Notepad++.

The plugin should:

- run inside a dockable Notepad++ panel;
- support real interactive terminal applications;
- integrate with the current Notepad++ file/folder context;
- support multiple shell types without bundling those shells;
- consume minimal resources when the terminal is closed or inactive;
- default to one active terminal session in v1;
- reuse WebView2 where practical;
- use Windows ConPTY instead of implementing terminal behavior manually.

The project should prioritize simplicity, stability, low memory usage, and ease of maintenance over feature completeness.

---

# 2. Core Design Principles

## 2.1 Lightweight First

NppTerminal must not bundle PowerShell, Git Bash, WSL distributions, or other shells.

The plugin only detects and launches shells already installed on the user's machine.

Example:

```text
NppTerminal
    |
    +-- pwsh.exe
    +-- cmd.exe
    +-- bash.exe
    +-- wsl.exe
```

Supporting more shell types should therefore have negligible impact on plugin size and no runtime memory cost when those shells are not running.

---

## 2.2 One Active Terminal by Default

Version 1 should support one active terminal session.

Avoid implementing multiple persistent terminal tabs in the MVP.

Reason:

- lower RAM usage;
- simpler process lifecycle management;
- simpler UI;
- fewer synchronization issues;
- easier shutdown behavior;
- easier debugging.

Multiple terminal tabs may be considered in a later release.

---

## 2.3 Native Terminal Backend

Use Windows ConPTY as the terminal backend.

Do not implement ANSI / VT terminal emulation manually.

Recommended architecture:

```text
Notepad++
   |
   +-- NppTerminal Plugin DLL
          |
          +-- Dockable panel
          |
          +-- WebView2
          |      |
          |      +-- xterm.js
          |
          +-- Windows ConPTY
                 |
                 +-- PowerShell
                 +-- cmd.exe
                 +-- Git Bash
                 +-- WSL
                 +-- Custom Shell
```

---

# 3. Primary User Stories

## US-01 — Open Terminal

As a Notepad++ user,

I want to open an integrated terminal inside Notepad++,

so that I do not need to switch to a separate terminal window.

---

## US-02 — Open Terminal in Current File Directory

As a user editing:

```text
C:\Projects\MyApp\src\main.cpp
```

when I open NppTerminal,

the terminal should start in:

```text
C:\Projects\MyApp\src
```

unless another working-directory rule has been configured.

---

## US-03 — Choose Shell

As a user,

I want to select from installed shells such as:

- PowerShell 7
- Command Prompt
- Git Bash
- WSL

without NppTerminal bundling any of them.

---

## US-04 — Run Interactive CLI Applications

As a user,

I want to run normal interactive terminal applications such as:

```text
git
npm
python
mvn
gradle
dotnet
pi
codex
claude
```

and have them behave like they do in a real terminal.

---

## US-05 — Toggle Terminal Quickly

As a user,

I want a keyboard shortcut to show/hide the terminal panel.

Suggested default:

```text
Ctrl + `
```

If that shortcut conflicts with Notepad++ or another plugin, it must remain configurable.

---

## US-06 — Open Terminal Here

As a user,

I want to open the terminal using the folder of the currently active file.

Potential UI entry:

```text
Tab context menu
    -> Open Terminal Here
```

This is desirable but may be implemented after the MVP if Notepad++ plugin API integration makes it disproportionately complex.

---

# 4. Functional Requirements

# 4.1 Dockable Terminal Panel

**REQ-F-001**

NppTerminal shall provide a dockable panel inside Notepad++.

Preferred default location:

```text
Bottom
```

The user should be able to:

- show the terminal;
- hide the terminal;
- resize the panel;
- dock/undock according to normal Notepad++ behavior where supported.

The implementation should use the normal Notepad++ docking API.

---

# 4.2 Terminal Rendering

**REQ-F-002**

The terminal UI should be rendered using:

```text
WebView2
+
xterm.js
```

The terminal must support at minimum:

- ANSI colors;
- cursor movement;
- command-line editing;
- Unicode;
- UTF-8;
- terminal resize;
- copy;
- paste;
- text selection;
- scrolling;
- interactive prompts.

The terminal should work with full-screen or semi-interactive terminal programs where ConPTY/xterm.js support permits.

---

# 4.3 ConPTY Backend

**REQ-F-003**

NppTerminal shall use Windows ConPTY to host shell processes.

Data flow:

```text
Shell stdout/stderr
        |
      ConPTY
        |
      C++
        |
 WebView2 message
        |
     xterm.js
```

Keyboard flow:

```text
Keyboard
   |
xterm.js
   |
WebView2 message
   |
Plugin C++
   |
ConPTY stdin
   |
Shell
```

The plugin must not parse or emulate terminal escape sequences itself.

---

# 4.4 Supported Shells

**REQ-F-004**

Version 1 should support:

1. PowerShell 7
2. Windows Command Prompt
3. Git Bash
4. WSL

Additional custom shells may be supported through configuration.

---

## 4.4.1 PowerShell 7

Typical executable:

```text
pwsh.exe
```

PowerShell 7 should be the preferred default shell when available.

---

## 4.4.2 Command Prompt

Typical executable:

```text
cmd.exe
```

Command Prompt should always be available on supported Windows versions.

---

## 4.4.3 Git Bash

Typical locations may include:

```text
C:\Program Files\Git\bin\bash.exe
C:\Program Files\Git\usr\bin\bash.exe
```

Do not assume a fixed path.

Use detection logic.

---

## 4.4.4 WSL

Typical launcher:

```text
wsl.exe
```

NppTerminal must not install or configure WSL.

It should only expose WSL if available.

---

# 4.5 Shell Auto-Detection

**REQ-F-005**

On startup or when opening shell settings, detect available shells.

Example:

```text
Available shells

✓ PowerShell 7
✓ Command Prompt
✓ Git Bash
✗ WSL
```

Unavailable shells should either:

- not be shown; or
- be shown disabled with a clear status.

Preferred MVP behavior:

**Only show detected shells.**

---

# 4.6 Shell Selection

**REQ-F-006**

The terminal toolbar should include a shell selector.

Example:

```text
PowerShell 7 ▼
```

Possible menu:

```text
PowerShell 7
Command Prompt
Git Bash
WSL
----------------
Terminal Settings...
```

Changing shell should terminate the existing terminal process cleanly before starting the new one.

Do not keep multiple shells alive simultaneously in v1.

---

# 4.7 Working Directory

**REQ-F-007**

When starting a new terminal, determine the working directory using the following priority:

```text
1. Explicit "Open Terminal Here" directory
2. Current active file directory
3. Configured default directory
4. Notepad++ process working directory
5. User home directory
```

Example:

Current file:

```text
D:\Code\MyProject\src\app.ts
```

Terminal starts in:

```text
D:\Code\MyProject\src
```

---

# 4.8 Active File Changes

**REQ-F-008**

Changing the active editor file must NOT automatically change the working directory of an existing terminal session.

Example:

Terminal currently at:

```text
C:\ProjectA
```

User switches editor tab to:

```text
D:\ProjectB\README.md
```

Existing terminal remains:

```text
C:\ProjectA
```

The working directory is determined only when a new terminal session starts or when the user explicitly requests a directory change.

This avoids surprising terminal behavior.

---

# 4.9 New / Restart Terminal

**REQ-F-009**

Provide an action to restart the current terminal.

Suggested toolbar:

```text
[PowerShell 7 ▼]  [Restart]  [Clear]  [Kill]
```

Restart behavior:

```text
terminate current shell
        |
start new shell
        |
same shell type
        |
current-file directory or configured restart rule
```

---

# 4.10 Kill Terminal

**REQ-F-010**

Provide a safe way to terminate the current shell process.

The implementation must clean up:

- shell process;
- ConPTY handles;
- pipes;
- worker threads;
- WebView-related session state.

Do not leave orphaned shell processes after Notepad++ closes.

---

# 4.11 Clear Terminal

**REQ-F-011**

Provide a Clear action.

Clearing the visual terminal must NOT kill the shell process.

Equivalent conceptually to clearing xterm.js buffer.

---

# 4.12 Resize

**REQ-F-012**

When the terminal panel is resized:

```text
Dock panel resize
       |
xterm.js calculates rows/columns
       |
send dimensions to plugin
       |
resize ConPTY
```

Resize should be debounced if needed to avoid excessive calls.

---

# 4.13 Copy / Paste

**REQ-F-013**

Support terminal copy/paste.

Recommended behavior:

```text
Ctrl + Shift + C -> Copy
Ctrl + Shift + V -> Paste
```

Do not override common shell control commands unnecessarily.

Example:

```text
Ctrl + C
```

must continue to work as terminal interrupt/SIGINT-style input where applicable rather than always acting as copy.

---

# 4.14 URL Detection

**REQ-F-014**

Clickable URLs are desirable.

This may be implemented using xterm.js link support.

Example:

```text
http://localhost:3000
https://github.com/...
```

Clicking a URL should open it using the user's default browser.

This feature may be excluded from the first minimal implementation if required.

---

# 4.15 Theme Integration

**REQ-F-015**

NppTerminal should visually follow Notepad++ theme where practical.

At minimum support:

- dark mode;
- light mode.

The terminal should avoid hard-coded theme assumptions.

Potential source:

```text
Notepad++ theme state
        |
Plugin
        |
WebView2
        |
CSS variables / xterm.js theme
```

Perfect color parity is not required for MVP.

---

# 5. User Interface Specification

Suggested MVP layout:

```text
+-----------------------------------------------------------+
| PowerShell 7 ▼     Restart    Clear    Kill               |
+-----------------------------------------------------------+
|                                                           |
| PS C:\Projects\my-app> npm test                           |
|                                                           |
| ✓ 28 tests passed                                        |
|                                                           |
| PS C:\Projects\my-app> █                                  |
|                                                           |
+-----------------------------------------------------------+
```

The toolbar should remain simple.

Avoid:

- large icons;
- unnecessary status panels;
- multiple nested menus;
- permanent sidebars;
- terminal tabs in MVP.

---

# 6. Settings

NppTerminal should provide minimal settings.

Recommended initial configuration:

```json
{
  "defaultShell": "powershell7",
  "startupDirectory": "currentFile",
  "fontFamily": "Cascadia Mono",
  "fontSize": 13,
  "scrollback": 5000,
  "confirmBeforeKill": false
}
```

Do not overbuild settings in v1.

---

# 7. Custom Shell Support

Custom shell support is useful but secondary.

Potential future configuration:

```json
{
  "shells": [
    {
      "id": "powershell7",
      "name": "PowerShell 7",
      "command": "pwsh.exe"
    },
    {
      "id": "gitbash",
      "name": "Git Bash",
      "command": "C:\\Program Files\\Git\\bin\\bash.exe"
    }
  ]
}
```

For MVP, built-in shell detection is sufficient.

Custom shell definitions may be implemented in Phase 2.

---

# 8. Performance Requirements

# 8.1 Plugin Idle State

**REQ-NF-001**

When the terminal has never been opened, NppTerminal should consume minimal additional resources.

The plugin should avoid:

- spawning shells;
- starting worker threads unnecessarily;
- initializing WebView2 earlier than needed if lazy initialization is practical.

Preferred behavior:

```text
Notepad++ starts
    |
NppTerminal DLL loaded
    |
No terminal panel opened
    |
No shell process
    |
No ConPTY session
```

---

# 8.2 Terminal Closed

**REQ-NF-002**

If the user closes/kills the terminal session:

- shell process must terminate;
- ConPTY session must be released;
- associated resources must be released.

Whether WebView2 is destroyed or retained may be determined based on stability/performance testing.

---

# 8.3 Shell Choice Must Not Increase Plugin Weight Significantly

**REQ-NF-003**

Supporting multiple shell options must be implemented by launching installed executables.

Do NOT bundle:

- PowerShell;
- Git;
- Bash;
- Linux distributions;
- WSL runtime.

Therefore:

```text
PowerShell support -> negligible plugin-size impact
CMD support        -> negligible plugin-size impact
Git Bash support   -> negligible plugin-size impact
WSL support        -> negligible plugin-size impact
```

---

# 8.4 Responsiveness

**REQ-NF-004**

Terminal input should feel immediate.

Avoid blocking the Notepad++ UI thread while:

- reading terminal output;
- writing terminal input;
- waiting for shell processes;
- resizing ConPTY.

Use background I/O where needed.

---

# 9. Process Lifecycle

Expected lifecycle:

```text
User opens terminal
        |
Create dock panel
        |
Initialize WebView2
        |
Initialize xterm.js
        |
Detect/select shell
        |
Create ConPTY
        |
Create shell process
        |
Start async I/O
        |
Terminal ready
```

Shutdown:

```text
User closes terminal / Notepad++ exits
        |
Stop input
        |
Terminate shell if needed
        |
Close process handles
        |
Close ConPTY
        |
Stop worker threads
        |
Release WebView resources
```

No orphan process should remain.

---

# 10. Error Handling

Errors must be shown clearly but unobtrusively.

Example:

```text
Unable to start PowerShell 7.

Executable not found:
pwsh.exe

Choose another terminal from the shell menu.
```

Possible failure cases:

- shell executable not found;
- WebView2 runtime unavailable;
- ConPTY initialization failure;
- pipe creation failure;
- shell process creation failure;
- permission error;
- invalid working directory;
- WSL unavailable.

Avoid crashing Notepad++ because of a terminal failure.

This is a critical requirement.

---

# 11. Security Requirements

## REQ-SEC-001

Do not execute commands automatically when merely opening the terminal.

---

## REQ-SEC-002

Do not transmit terminal input/output to remote services.

NppTerminal itself must operate locally.

---

## REQ-SEC-003

Do not log terminal contents, credentials, environment variables, or command history unless the user explicitly enables diagnostic logging.

---

## REQ-SEC-004

If diagnostic logs are implemented, redact or avoid storing sensitive terminal content.

---

## REQ-SEC-005

Do not inject arbitrary selected editor text into the shell without an explicit user action.

---

# 12. Notepad++ Context Integration

One of NppTerminal's main advantages over an external terminal should be editor awareness.

Potential context values:

```text
${file}
${fileDir}
${fileName}
${fileNameWithoutExtension}
${notepadDir}
```

Examples:

```text
python "${file}"
```

```text
git diff "${file}"
```

```text
cd "${fileDir}"
```

These variables are not necessarily required in MVP command templating, but the architecture should not prevent them from being added later.

---

# 13. AI / Coding-Agent Use Cases

NppTerminal should work correctly with CLI coding agents.

Examples:

```text
pi
codex
claude
gemini
```

Typical workflow:

```text
Notepad++
   |
   +-- Editor: requirement.md
   |
   +-- NppTerminal
          |
          +-- codex
                 |
                 +-- analyze project
                 +-- modify files
                 +-- run tests
```

No special AI integration is required for v1.

The requirement is simply that interactive CLI applications work correctly through ConPTY.

---

# 14. Proposed Technical Components

Recommended stack:

| Component | Responsibility |
|---|---|
| C++ | Native Notepad++ plugin |
| Notepad++ Plugin API | Plugin lifecycle and docking |
| WebView2 | Terminal UI host |
| xterm.js | Terminal rendering |
| xterm-addon-fit | Terminal fitting/resizing |
| Windows ConPTY | Pseudoterminal backend |
| Win32 Process APIs | Shell creation and lifecycle |
| JSON/config file | User settings |

Avoid introducing Node.js as a runtime dependency for the installed plugin.

JavaScript may be bundled as static frontend assets inside the plugin package.

---

# 15. Suggested Internal Architecture

```text
src/
|
+-- PluginMain
|     +-- Notepad++ plugin lifecycle
|     +-- menu registration
|
+-- TerminalPanel
|     +-- dockable window
|     +-- toolbar
|     +-- WebView host
|
+-- TerminalSession
|     +-- session state
|     +-- start()
|     +-- stop()
|     +-- restart()
|
+-- ConPtyHost
|     +-- create pseudo console
|     +-- pipes
|     +-- resize
|     +-- cleanup
|
+-- ProcessHost
|     +-- CreateProcess
|     +-- shell arguments
|     +-- process lifecycle
|
+-- ShellDetector
|     +-- PowerShell detection
|     +-- cmd detection
|     +-- Git Bash detection
|     +-- WSL detection
|
+-- ContextProvider
|     +-- active file
|     +-- file directory
|     +-- Notepad++ directory
|
+-- Settings
      +-- load
      +-- save
```

Frontend:

```text
web/
|
+-- index.html
+-- terminal.js
+-- terminal.css
+-- xterm/
+-- icons/
```

Exact structure may be adapted to the existing Notepad++ plugin template.

---

# 16. Threading Model

The Notepad++ UI thread must not be blocked by terminal I/O.

Suggested approach:

```text
UI Thread
    |
    +-- WebView / panel events
    +-- shell commands
    +-- resize request
```

Terminal I/O:

```text
Background reader
    |
Read ConPTY output
    |
marshal data safely
    |
WebView2 postMessage / ExecuteScript
```

Input:

```text
WebView2 input event
    |
Plugin event handler
    |
write to ConPTY input pipe
```

Use a clean cancellation strategy during shutdown.

---

# 17. MVP Scope

The first usable release should include only:

- Notepad++ dockable terminal panel;
- bottom docking;
- WebView2;
- xterm.js;
- Windows ConPTY;
- PowerShell 7;
- cmd.exe;
- Git Bash;
- WSL;
- automatic shell detection;
- shell selector;
- one active terminal session;
- current-file-directory startup;
- restart terminal;
- kill terminal;
- clear terminal;
- copy/paste;
- terminal resize;
- ANSI colors;
- UTF-8;
- dark/light theme basics;
- safe cleanup on plugin/Notepad++ exit.

This is enough for a useful first release.

---

# 18. Explicitly Out of Scope for MVP

Do NOT implement initially:

- multiple simultaneous terminal tabs;
- split terminals;
- SSH client;
- built-in file explorer;
- built-in Git UI;
- terminal history database;
- cloud synchronization;
- shell installation;
- WSL installation;
- PowerShell installation;
- AI agent UI;
- command palette;
- task runner;
- remote terminal server;
- terminal sharing;
- session restore;
- workspace management;
- complex profiles;
- environment-variable editor;
- custom terminal emulator;
- built-in ANSI parser.

These features can cause scope creep and unnecessary resource usage.

---

# 19. Future Phase 2 Features

Potential additions:

## 19.1 Open Terminal Here

Context menu:

```text
Open Terminal Here
```

---

## 19.2 Custom Shell Profiles

Example:

```text
PowerShell 7
Command Prompt
Git Bash
WSL Ubuntu
WSL Debian
Developer PowerShell
Custom...
```

---

## 19.3 Command Templates

Examples:

```text
Run Current Python File
Run Current Node File
Git Diff Current File
Open Pi Agent Here
Open Codex Here
```

---

## 19.4 Notepad++ Variables

Example:

```text
${file}
${fileDir}
${selectedText}
```

---

## 19.5 Multiple Terminals

Only after resource measurements prove acceptable.

Possible:

```text
Terminal 1 | Terminal 2 | +
```

But this should remain optional.

---

# 20. Future Phase 3 Features

Possible advanced capabilities:

- terminal search;
- configurable profiles;
- session restore;
- task definitions;
- hyperlink integration with Notepad++ files;
- parse `file:line` terminal output;
- Ctrl+click stack traces;
- clickable compilation errors;
- optional terminal split;
- optional AI-agent shortcuts.

---

# 21. Acceptance Criteria

## AC-001

Given Notepad++ is running,

when the user opens NppTerminal,

then a docked terminal panel appears at the bottom.

---

## AC-002

Given PowerShell 7 is installed,

when the terminal opens,

then PowerShell 7 can be launched inside the panel.

---

## AC-003

Given PowerShell 7 is not installed,

when shells are detected,

then PowerShell 7 is not selected as an invalid default.

---

## AC-004

Given Git Bash is installed,

then Git Bash appears in the shell selector.

---

## AC-005

Given WSL is installed,

then WSL appears in the shell selector.

---

## AC-006

Given the active file is:

```text
C:\Work\TestProject\src\main.py
```

when a new terminal starts,

then its initial directory is:

```text
C:\Work\TestProject\src
```

---

## AC-007

Given an active terminal is running,

when the user changes editor tabs,

then the terminal working directory does not automatically change.

---

## AC-008

Given the user runs:

```text
python
```

then interactive input/output works correctly.

---

## AC-009

Given the user runs:

```text
npm run dev
```

then streaming colored output renders correctly.

---

## AC-010

Given the user runs an interactive CLI such as:

```text
codex
```

or:

```text
pi
```

then keyboard interaction, screen updates, colors, and resize behave correctly through ConPTY.

---

## AC-011

Given the terminal panel is resized,

then terminal rows/columns update correctly without corrupting output.

---

## AC-012

Given the user closes Notepad++,

then no NppTerminal-created shell process remains running.

---

## AC-013

Given the terminal is not open,

then no shell process or ConPTY session exists.

---

## AC-014

Supporting four shell types must not require bundling those shell runtimes into NppTerminal.

---

# 22. Testing Requirements

Codex should create automated tests where practical and provide manual test instructions for UI/ConPTY behavior.

Test categories:

## Shell Detection

Test:

- PowerShell installed/not installed;
- Git Bash installed/not installed;
- WSL available/not available;
- cmd available.

---

## Working Directory

Test:

- saved current file;
- new/unsaved file;
- deleted file directory;
- invalid path;
- network path if practical.

---

## Terminal Lifecycle

Test:

```text
start
restart
kill
reopen
close Notepad++
```

Ensure no handles/processes leak.

---

## Interactive Applications

Manual test:

```text
python
node
git
npm
ping
pi
codex
```

---

## Unicode

Test:

```text
Tiếng Việt
日本語
中文
emoji
```

---

## Resize

Repeatedly resize the terminal panel during streaming output.

---

## Stress

Example:

```text
ping 127.0.0.1 -t
```

or another continuous-output command.

Resize, hide/show, clear, and kill while output is active.

---

# 23. Resource Targets

These are directional rather than strict limits.

NppTerminal should aim for:

- negligible CPU when idle;
- no shell process when no session exists;
- one shell process by default;
- no bundled shell runtimes;
- lazy initialization where practical;
- no Node.js background runtime;
- no unnecessary polling loops.

Measure memory after implementation rather than optimizing prematurely.

---

# 24. Packaging

Expected package conceptually:

```text
plugins/
└── NppTerminal/
    ├── NppTerminal.dll
    ├── web/
    │   ├── index.html
    │   ├── terminal.js
    │   ├── terminal.css
    │   └── xterm assets
    └── config/
```

Final packaging should follow current Notepad++ plugin conventions.

Avoid unnecessary dependencies.

---

# 25. Compatibility

Initial target:

```text
Windows 10
Windows 11
64-bit Notepad++
```

32-bit support is optional unless it can be added with little complexity.

ConPTY availability must be considered when determining minimum Windows build support.

---

# 26. Definition of Done for v1

Version 1 is done when:

1. NppTerminal loads safely in Notepad++.
2. Terminal panel docks correctly.
3. WebView2/xterm.js renders correctly.
4. ConPTY starts successfully.
5. PowerShell 7 works.
6. cmd works.
7. Git Bash works when installed.
8. WSL works when installed.
9. Shell auto-detection works.
10. Shell switching works.
11. Current-file working directory works.
12. Interactive applications work.
13. Resize works.
14. Copy/paste works.
15. UTF-8 works.
16. Dark/light appearance is acceptable.
17. Kill/restart works.
18. Notepad++ remains responsive.
19. No terminal child process remains after exit.
20. README includes install/use/troubleshooting instructions.

---

# 27. Implementation Strategy for Codex

Codex should implement incrementally.

Recommended order:

```text
Step 1
Create minimal Notepad++ plugin skeleton.

Step 2
Create dockable bottom panel.

Step 3
Host WebView2 inside panel.

Step 4
Load a minimal xterm.js terminal.

Step 5
Implement ConPTY host.

Step 6
Launch cmd.exe first.

Step 7
Connect ConPTY output to xterm.js.

Step 8
Connect xterm.js keyboard input to ConPTY.

Step 9
Implement terminal resize.

Step 10
Implement safe process cleanup.

Step 11
Add PowerShell 7 detection.

Step 12
Add Git Bash detection.

Step 13
Add WSL detection.

Step 14
Add shell selector.

Step 15
Add current-file working directory integration.

Step 16
Add restart/clear/kill.

Step 17
Add theme support.

Step 18
Test interactive CLI applications.

Step 19
Measure CPU/RAM/process behavior.

Step 20
Polish packaging and README.
```

Do not implement all features simultaneously.

The first technical milestone should simply be:

```text
Notepad++
   |
dock panel
   |
xterm.js
   |
ConPTY
   |
cmd.exe
```

Once that works reliably, expand shell support.

---

# 28. Engineering Rules for Codex

When implementing this project:

1. Prefer the simplest reliable architecture.
2. Do not introduce Node.js as an installed runtime dependency.
3. Do not bundle shell runtimes.
4. Do not implement terminal emulation manually.
5. Use ConPTY.
6. Use xterm.js.
7. Keep one terminal session in MVP.
8. Never block the Notepad++ UI thread with terminal reads.
9. Use RAII for Win32 handles where practical.
10. Ensure shell processes are cleaned up.
11. Handle plugin unload safely.
12. Keep external dependencies minimal.
13. Do not redesign unrelated parts of the existing plugin codebase.
14. Reuse existing WebView2 infrastructure if the project already contains a mature implementation.
15. Preserve compatibility with the existing Notepad++ plugin architecture.
16. Add logging only where useful for diagnostics.
17. Do not log terminal content by default.
18. Build incrementally and test after each milestone.

---

# 29. Suggested First Codex Task

The first implementation task should be intentionally narrow:

> Build a proof of concept NppTerminal panel inside Notepad++ using the existing plugin architecture. Create a bottom dockable panel containing WebView2 and xterm.js. Use Windows ConPTY to launch `cmd.exe`. Connect terminal input, output, and resize. Do not add multiple shells or settings yet. Ensure closing the terminal and Notepad++ cleans up the child process and all ConPTY resources. Keep Notepad++ responsive.

Acceptance for this first task:

```text
Open NppTerminal
        |
cmd.exe appears
        |
type "dir"
        |
output renders
        |
resize panel
        |
terminal adapts
        |
close Notepad++
        |
no cmd.exe remains
```

Only after this proof of concept is stable should PowerShell, Git Bash, WSL, shell selection, and additional UI be added.

---

# 30. Final Product Direction

NppTerminal is not intended to transform Notepad++ into a full IDE.

Its intended positioning is:

```text
Notepad++
+
good preview capabilities
+
lightweight integrated terminal
+
optional external CLI tools / coding agents
```

This enables workflows such as:

```text
Edit
   ->
Preview
   ->
Run / Test / Git / Pi / Codex
```

without requiring a heavyweight development environment.

The product should remain:

**small, fast, local, understandable, and optional.**
