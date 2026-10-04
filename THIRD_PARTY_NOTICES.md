# Third-party notices

NppTerminal source is licensed under GPL-3.0-or-later; see `LICENSE`.
When distributing binaries, provide the corresponding source and retain these notices.
This development package is not a completed v1 release.

| Component | Pinned source | Notices |
| --- | --- | --- |
| Notepad++ plugin interface and docking wrappers | [official template](https://github.com/npp-plugins/plugintemplate/tree/27b7077ba89766b3a5a136a3d71af3a0cccc7d2a) | Plugin/docking file headers specify GPL-3.0-or-later. The upstream template also includes a GPL-2.0 text, preserved in `third_party/notepadpp/LICENSE.txt`. |
| Notepad++ public message definitions | [v8.9.8.1](https://github.com/notepad-plus-plus/notepad-plus-plus/blob/v8.9.8.1/PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h) | Upstream file header retained. |
| Scintilla interface headers | Included in the pinned template | Neil Hodgson's permissive license, `third_party/notepadpp/Scintilla.LICENSE.txt`. |
| nlohmann/json | [v3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) | MIT, `third_party/json/LICENSE.MIT`. |
| Microsoft WebView2 SDK and static loader | [1.0.4258.31](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4258.31) | SDK `LICENSE.txt` and `NOTICE.txt`, copied into package `licenses/`. |
| @xterm/xterm | 6.0.0 | MIT, `web/vendor/xterm.LICENSE`. |
| @xterm/addon-fit | 0.11.0 | MIT, `web/vendor/addon-fit.LICENSE`. |

No shell, WSL distribution, Node runtime, or Evergreen runtime is bundled.
