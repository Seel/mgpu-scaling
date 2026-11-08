# Notes for agents working in this repo

Windows host. Use **PowerShell 7** (`pwsh.exe`) for every shell command:

```powershell
pwsh -NoProfile -Command "..."
```

`powershell.exe` is Windows PowerShell 5.1 and is a different product — installing PowerShell 7
does not change what that name runs. They differ in ways that break scripts: in 5.1 a native
command writing to stderr becomes a terminating `NativeCommandError` while
`$ErrorActionPreference = 'Stop'`, even with `2>$null`.

`build-local.ps1` (at the repository root, with `overlay/` beside it) re-runs itself under `pwsh`
when started from 5.1, so the build is safe either way, but do not write new scripts that assume
5.1 semantics.
