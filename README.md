# claude-here

Adds two entries to the **new Windows 11 context menu** (the short one, not only
"Show more options"):

- **Open in Claude Code**: opens `claude` (the Claude Code CLI) in Windows Terminal, in the
  folder you clicked.
- **Open in Claude Desktop**: starts a new Claude Code session on that folder in the Claude
  desktop app.

Both show up when you right-click a folder and when you right-click empty space inside one.
The titles follow the Windows display language: Turkish on a Turkish UI, English otherwise.

> Unofficial. Not affiliated with or endorsed by Anthropic.

## How it behaves

**Open in Claude Code** uses Windows Terminal's **default profile**, so the tab gets that
profile's icon, colours and title, and `claude` starts *inside* the profile's shell:

| Default profile | What runs |
| --- | --- |
| PowerShell 7 / Windows PowerShell | `pwsh -NoExit -Command "& 'claude.exe'"` |
| Command Prompt | `cmd /k "claude.exe"` |
| anything else (WSL, Git Bash, …) | `claude.exe` directly, with the profile's look |

Your shell profile loads, and when `claude` exits you are left in the shell in that folder.
The default profile is read on every click, so changing it in Windows Terminal takes effect
at once. Without Windows Terminal, `claude` opens in the default console.

**Open in Claude Desktop** opens `claude://code/new?folder=<path>`. The desktop app asks for
confirmation every time a folder comes from another app; that is the app's own safety
check, and there is no setting to turn it off. The entry is hidden when the desktop app is
not installed, and the terminal entry is hidden when `claude` is not found.

## How it works

A small C++ COM DLL (`src/ClaudeHere.cpp`) implements `IExplorerCommand`. Each command is
registered by its own **sparse package** (`package/AppxManifest.template.xml`). This is the
same mechanism Windows Terminal uses for "Open in Terminal". The DLL runs in a `dllhost.exe`
surrogate, not inside Explorer.

The two commands are separate packages so Windows does not fold them into one submenu, and
each can be moved in or out of the main menu on its own under **Settings → Personalization
→ Context menu → App extensions** (on builds that have that page).

## Requirements

- Windows 11 with **Developer Mode on**, because the packages are registered unsigned.
- Visual Studio 2022 with the C++ tools, and the Windows 11 SDK
  (`winget install Microsoft.WindowsSDK.10.0.26100`).
- Claude Code CLI (`%USERPROFILE%\.local\bin\claude.exe` or on `PATH`), and optionally the
  Claude desktop app.

## Install

Run from PowerShell 7 in the repo folder:

```powershell
.\build.ps1      # builds build\ClaudeHere.dll and build\ClaudeHereStub.exe
.\install.ps1    # copies them to %LOCALAPPDATA%\ClaudeHere and registers the packages
```

Run both again to update. To remove everything:

```powershell
.\uninstall.ps1
```

If an entry does not appear right away, check the **App extensions** settings page, or sign
out and back in.

## License

[MIT](LICENSE)
