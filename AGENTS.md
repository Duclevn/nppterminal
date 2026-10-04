# NppTerminal project instructions

- `VERSION` is the authoritative three-part project version. Keep `src/Version.h`, `src/NppTerminal.rc`, and the npm metadata under `web/` synchronized through `scripts/version.ps1`.
- Use `scripts/build.ps1` for native builds. A normal build reserves the next patch version; use `-VersionBump Minor` for a feature or substantial refactor and `-VersionBump Major` only for an explicit major release request. A failed native build consumes its reserved version.
- Use `scripts/package.ps1` after a successful build. Packaging does not bump the version and must reject a DLL whose embedded version differs from `VERSION`.
- Report the emitted version and the dated validation checks with each build/package handoff.
