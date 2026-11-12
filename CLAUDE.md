# CLAUDE.md

Windows 11 context menu shell extension (`IExplorerCommand`) adding "Open in Claude Code"
and "Open in Claude Desktop". Behaviour and install steps are in `README.md`.

## Language

The repository is public: code, comments, docs and commit messages (Conventional Commits)
are in English. This overrides the user's global preference for Turkish docs and commits.
Exception: the Turkish menu titles in `src/ClaudeHere.cpp` and package names in
`install.ps1`. The first commits are in Turkish; history is not rewritten. Replies to the
user in chat stay in Turkish.

## Commands

```powershell
.\build.ps1      # MSVC via vcvars64, outputs to build\
.\install.ps1    # copies to %LOCALAPPDATA%\ClaudeHere, registers both sparse packages
.\uninstall.ps1  # removes the packages and the install folder
```

There are no automated tests. The COM side was checked with a throwaway probe that calls
`CoCreateInstance` + `GetTitle`/`GetIcon`/`GetState`/`Invoke` on both CLSIDs; the menu itself
can only be checked by hand.

## Notes

- Packages are unsigned and registered with `Add-AppxPackage -Register -ExternalLocation`,
  which needs Developer Mode. Without it, signing would need makeappx + a certificate
  trusted in TrustedPeople (admin).
- Register from `%LOCALAPPDATA%\ClaudeHere`, never from the repo: the loaded DLL gets locked.
  `install.ps1` first stops the `dllhost` processes holding it.
- The two commands are deliberately separate packages (`ClaudeHere.Terminal`,
  `ClaudeHere.Desktop`): Windows may group several verbs of one package into a submenu.
- The CLSIDs live in both `src/ClaudeHere.cpp` and `install.ps1`; change them together.
- Menu icons: `GetIcon` returns `icon.ico` next to the DLL, falling back to the exe's icon 0.
  `install.ps1` generates it from each exe, cropped to the visible area, because the CLI's
  own icon has about 2 px of padding at 32 px and the desktop app's has none (they looked
  different in size). No icon art is committed to the repo.
- Language: the menu title uses `GetUserDefaultUILanguage` (the display language Explorer
  uses); the package name uses `Get-UICulture` at install time. Windows Terminal follows the
  preferred-languages list instead, so its own entry can be in a different language.
- Terminal entry: reads Windows Terminal's `settings.json` on every click (JSONC: comments
  and trailing commas are stripped, then `Windows.Data.Json`), passes `defaultProfile` with
  `-p` and runs claude inside the profile's shell. If the profile already carries a command
  (`-Command`, `/c`, …), claude runs directly instead.
- Desktop entry: `claude://code/new?folder=<encodeURIComponent>` is the format the desktop
  app's own `ccd` alias produces (read from its app.asar). The app adds `src=external` and
  always asks for confirmation; this cannot be bypassed. Its "Trust this workspace?" answer
  is stored in the CLI's `~/.claude.json` (`projects[path].hasTrustDialogAccepted`).
- Recent sessions submenu: transcripts are `%USERPROFILE%\.claude\projects\<dir>\<uuid>.jsonl`,
  where `<dir>` is the folder path with every non-alphanumeric character replaced by `-` (checked
  on ASCII paths only). `cwd` and `aiTitle` are read from the first 512 KB. The submenu needs
  `GetState` to see the clicked folder first, because `GetFlags` and `EnumSubCommands` get no items.
  `ECF_HASSPLITBUTTON` was tried and dropped: the new menu shows the arrow but never invokes the
  root, so "New session" stays inside the submenu.
- Desktop resume: `claude://resume?session=<uuid>` (host `resume`, read from app.asar). It imports
  the CLI transcript into a new desktop session with the history; the CLI session is untouched.
