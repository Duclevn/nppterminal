# NppTerminal

A local, single-session terminal for Notepad++ x64, using a bottom dock, WebView2, xterm.js and Windows ConPTY.

This build includes installed PowerShell 7, Command Prompt, Git Bash and WSL discovery, directory selection, settings, themes and native clipboard shortcuts. Development candidate **0.3.26** fixes Ctrl+C delivery and passed actual Python/PowerShell interrupt checks on 4 October 2026. Its standard run passed **18 of 19 groups**; concurrent profile cleanup failed. The earlier 0.3.23 active-memory failure (168.65–170.63 MiB versus 150 MiB) remains unresolved; current memory/latency/30-minute and clean-PC/full interactive checks remain open. This is a development package, not a certified release. Supported scope is Windows 11 x64. See `docs/implementation-status.md` for dated evidence and limits.

## Build

Use Windows with Visual Studio 2022 C++ build tools (v143), a Windows SDK, and Node/npm for building static assets. The installed plugin does not use Node.

```powershell
./scripts/build.ps1
npm --prefix web test
./out/x64/Release/NppTerminal.Tests.exe --unit
./out/x64/Release/NppTerminal.Tests.exe --conpty
./out/x64/Release/NppTerminal.Tests.exe --webview-security
./scripts/validate.ps1
./scripts/package.ps1
./scripts/inspect-package.ps1
```

`VERSION` is the authoritative three-part version. Each `scripts/build.ps1` run reserves the next patch version, including a failed native build. Use `./scripts/build.ps1 -VersionBump Minor` for a feature or substantial refactor, or `-VersionBump Major` for an explicit major-version request; both reset lower components to zero. `scripts/package.ps1` does not increment the version, validates the DLL and session helper's embedded versions against `VERSION`, and writes a ZIP named with that version. Direct MSBuild builds the currently stamped version and does not reserve a new one.

`inspect-package.ps1` checks the current ZIP against its exact 26-file manifest and current binary, asset, license and documentation hashes. Use `-OutputPath <report.json>` to save its dated inspection result in an existing directory. It does not extract or modify the archive.

`package-source.ps1` writes a curated corresponding-source ZIP and a dated per-file SHA-256 inspection report under `out`. It includes native projects, sources, tests, scripts, docs, dependency locks and shipping web assets; generated builds, downloaded SDKs and node_modules are excluded. Extract it into a fresh directory and run `scripts/build.ps1` to bootstrap and rebuild. That build reserves the next version as usual; the archive records its input version.

The Phase 1 broker refactor adds `NppTerminalBroker.exe` to the installed folder. `NppTerminalBroker.Tests.exe` is a test build and is excluded from the package. `validate.ps1` saves dated logs and exact exit results under `out/validation`; `-IncludeLongStream` adds the thirty-minute native producer, and the required `--webview-security` group checks the WebView security policy. See `docs/shutdown-ownership.md` for the refactor contract and `docs/implementation-status.md` for its actual validation state.

`bootstrap.ps1` downloads the pinned WebView2 SDK, verifies its SHA-256, runs `npm ci`, and copies pinned xterm assets. Native public headers and the JSON library are vendored. Dependency pins are recorded in `dependencies.lock.json`.

## Install and use the development package

Target Notepad++ 8.9.8.1 x64 on Windows 11, with Microsoft's [Evergreen WebView2 Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/) installed. Windows 10 is outside the supported release scope.

Close the intended Notepad++ instance. Extract the entire `NppTerminal` folder from the generated ZIP into its `plugins` directory. The result must include `plugins/NppTerminal/NppTerminal.dll`, `plugins/NppTerminal/NppTerminalBroker.exe` and `plugins/NppTerminal/web/index.html`. A DLL-only installation is insufficient. Prefer a disposable portable Notepad++ instance for this development build.

Open **Plugins → NppTerminal → Toggle Terminal**, or press **Ctrl+Alt+T**. First opening initializes the renderer, detects installed shells and starts the configured shell once the page reports a usable grid. Loading the plugin starts no shell or browser. The toolbar provides shell selection, Start/Retry, Restart, Clear, Kill, Refresh and Settings. Refresh updates the cached catalog without restarting a live shell. Explicit shell selections report launch errors; an unavailable configured default falls back to installed PowerShell 7, then Command Prompt.

New sessions use the saved active file's directory, configured default directory, host working directory, then user home. **Open Terminal Here** starts in the saved active file's directory and replaces an existing session. Unsupported explicit paths are reported; Command Prompt skips automatic UNC candidates. Existing sessions keep their directory when editor tabs change.

**Terminal Settings** controls the default shell/directory, font family/size, scrollback and optional confirmation before Kill, Restart or shell replacement. Font and scrollback changes apply live; shell/directory preferences apply at the next start. Settings are stored as `NppTerminal.json` in the configuration directory reported by Notepad++. Save replaces the file atomically; Cancel preserves it, and Reset to Defaults changes the form until Save. Malformed settings use defaults without rewriting the file automatically.

Use **Ctrl+Shift+C** to copy the terminal selection and **Ctrl+Shift+V** to paste Unicode text through the native clipboard (maximum 64 KiB). **Ctrl+C** remains an interrupt. Paste uses xterm's bracketed-paste handling when the shell enables it. Shell OSC52 clipboard requests remain blocked.

If another plugin already uses **Ctrl+Alt+T**, change one command in **Settings → Shortcut Mapper → Plugin commands**. Select the conflicting command and choose **Modify** or **Clear**, then assign the shortcut to **NppTerminal → Toggle Terminal**. Notepad++ stores shortcut overrides in its configuration, so an existing override can take precedence over this default after updating the plugin.

Toggle and the panel X hide the panel and preserve the running session. Kill stops it and releases its renderer. Clear clears the visible terminal without restarting the shell. Natural shell exit leaves the final output visible and read-only; use Restart to run another shell. A restored dock stays idle until Start or Toggle. The broker refactor moves terminal workers into a contained helper and uses a bounded stop followed by job termination if needed. WebView asynchronous callbacks independently require a process-lifetime module pin after first use. WebView startup also fails closed when the runtime lacks the required frame-navigation and download security capability; install or update Evergreen WebView2 and retry. Its full callback and profile-cleanup matrix remains under validation; see `docs/shutdown-ownership.md`.

No shell, WSL distribution, Evergreen runtime, or Node runtime is bundled. The plugin adds no startup commands or editor text, sends no terminal contents to remote services, and does not log terminal contents. A shell may run its own normal startup configuration.

WSL cleanup covers the attached session; detached Linux work and shared services are outside that promise. Tests identify their own Linux shell and children by PID plus `/proc` start time before verifying their disappearance. They do not terminate a distribution or VM. URL detection (F-014) is deferred under the plan's optional security/maintenance gate; terminal links do not open a browser.

## Troubleshooting

- Missing renderer or required WebView security capability: install or update Evergreen WebView2 using Microsoft's instructions, then Retry. The plugin does not install it automatically.
- Blank/error page: verify the entire `web` folder and its `vendor` assets are beside the DLL.
- Shell launch failure: check the native status message and use Refresh after installing or removing a shell. Command Prompt is resolved from the Windows system directory. Git Bash launches `bash.exe` directly; no Mintty or shell runtime is bundled. WSL is shown only after bounded capability and installed-distribution checks.
- Clipboard writes requested by shell OSC52 sequences are blocked. Browser navigation, downloads, permission requests, and remote terminal transport are disabled.
- Unexpected lifecycle behavior: report the exact Windows, Notepad++, and WebView2 versions and the action sequence. Avoid sharing terminal commands, output, credentials, or environment variables.

## Development checks

Follow `docs/manual-tests.md` for docking, rendering, responsiveness, shutdown, and resource checks. A passing build and harness do not certify the full UI or all supported operating systems. Release remains gated by the implementation plan.

Source license: GPL-3.0-or-later. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
