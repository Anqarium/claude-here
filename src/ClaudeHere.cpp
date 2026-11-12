// IExplorerCommand server that adds two commands to the Windows 11 context menu:
//   - Terminal: opens `claude` (Claude Code CLI) in Windows Terminal, in the clicked folder.
//   - Desktop: starts a new Claude Code session on that folder in the Claude desktop app.
// Each command is registered by its own sparse package (package/*.xml); the DLL runs in dllhost.exe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <appmodel.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <vector>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

// {0F55B8AB-4947-4EAB-B1F7-4E8C0DB27AC9}
static const CLSID CLSID_ClaudeTerminal = {0x0f55b8ab, 0x4947, 0x4eab, {0xb1, 0xf7, 0x4e, 0x8c, 0x0d, 0xb2, 0x7a, 0xc9}};
// {59847620-2A77-4B45-AD83-EF512E443B3B}
static const CLSID CLSID_ClaudeDesktop = {0x59847620, 0x2a77, 0x4b45, {0xad, 0x83, 0xef, 0x51, 0x2e, 0x44, 0x3b, 0x3b}};

// Package family of the Claude desktop app (the publisher hash is fixed across versions).
static const wchar_t kDesktopFamily[] = L"Claude_pzs8sxrjxfjjc";

static LONG g_objects = 0;
static LONG g_locks = 0;
static HMODULE g_module = nullptr;

enum class Kind { Terminal, Desktop };

static std::wstring EnvPath(const wchar_t* var, const wchar_t* tail) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf) + tail;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Claude Code CLI: the native installer's default location first, then PATH.
static std::wstring ClaudeCliPath() {
    std::wstring p = EnvPath(L"USERPROFILE", L"\\.local\\bin\\claude.exe");
    if (!p.empty() && FileExists(p)) return p;
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, L"claude.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) return buf;
    return L"";
}

// Path of the Claude desktop app's exe; empty when it is not installed.
static std::wstring DesktopExePath() {
    UINT32 count = 0, bufLen = 0;
    LONG rc = FindPackagesByPackageFamily(kDesktopFamily, PACKAGE_FILTER_HEAD, &count, nullptr, &bufLen, nullptr, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0) return L"";
    std::vector<PWSTR> names(count);
    std::vector<wchar_t> buf(bufLen);
    if (FindPackagesByPackageFamily(kDesktopFamily, PACKAGE_FILTER_HEAD, &count, names.data(), &bufLen, buf.data(), nullptr) != ERROR_SUCCESS)
        return L"";
    UINT32 pathLen = 0;
    if (GetPackagePathByFullName(names[0], &pathLen, nullptr) != ERROR_INSUFFICIENT_BUFFER) return L"";
    std::vector<wchar_t> path(pathLen);
    if (GetPackagePathByFullName(names[0], &pathLen, path.data()) != ERROR_SUCCESS) return L"";
    std::wstring exe = std::wstring(path.data()) + L"\\app\\Claude.exe";
    return FileExists(exe) ? exe : L"";
}

static HRESULT PathOfItem(IShellItem* item, std::wstring& out) {
    PWSTR p = nullptr;
    HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &p);
    if (SUCCEEDED(hr)) {
        out = p;
        CoTaskMemFree(p);
    }
    return hr;
}

// The clicked folder. On a folder background the item array may be empty; then the
// window's own folder is taken from the site via IFolderView.
static HRESULT TargetFolder(IShellItemArray* items, IUnknown* site, std::wstring& out) {
    if (items) {
        DWORD count = 0;
        if (SUCCEEDED(items->GetCount(&count)) && count > 0) {
            IShellItem* item = nullptr;
            HRESULT hr = items->GetItemAt(0, &item);
            if (SUCCEEDED(hr)) {
                hr = PathOfItem(item, out);
                item->Release();
            }
            return hr;
        }
    }
    if (!site) return E_FAIL;
    IServiceProvider* sp = nullptr;
    HRESULT hr = site->QueryInterface(IID_PPV_ARGS(&sp));
    if (FAILED(hr)) return hr;
    IFolderView* fv = nullptr;
    hr = sp->QueryService(SID_SFolderView, IID_PPV_ARGS(&fv));
    sp->Release();
    if (FAILED(hr)) return hr;
    IShellItem* folder = nullptr;
    hr = fv->GetFolder(IID_PPV_ARGS(&folder));
    fv->Release();
    if (FAILED(hr)) return hr;
    hr = PathOfItem(folder, out);
    folder->Release();
    return hr;
}

// Equivalent of encodeURIComponent (UTF-8).
static std::wstring UriEncode(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
    static const char hex[] = "0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : u) {
        if (isalnum(c) || strchr("-_.!~*'()", c)) {
            out += (wchar_t)c;
        } else {
            out += L'%';
            out += (wchar_t)hex[c >> 4];
            out += (wchar_t)hex[c & 15];
        }
    }
    return out;
}

// --- Recent Claude Code sessions of a folder --------------------------------

// Claude Code keeps the transcripts of a project in %USERPROFILE%\.claude\projects\<name>\, where
// <name> is the project path with every non-alphanumeric character replaced by '-'.
constexpr size_t kMaxSessions = 5;

struct Session {
    std::wstring id;     // session UUID: the .jsonl file name without its extension
    std::wstring label;  // "<AI title> - dd.MM HH:mm" (or only the time when there is no title)
};

static bool IsUuid(const std::wstring& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); i++) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        bool ok = dash ? s[i] == L'-' : (s[i] < 128 && isxdigit((int)s[i]));
        if (!ok) return false;
    }
    return true;
}

static std::wstring EncodeProjectDir(const std::wstring& folder) {
    std::wstring out = folder;
    for (auto& c : out)
        if (!(c < 128 && isalnum((int)c))) c = L'-';
    return out;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// The four hex digits at text[i..i+3] as a code unit, or -1 when they are missing or invalid.
static int Hex4(const std::string& text, size_t i) {
    if (i + 4 > text.size()) return -1;
    int cp = 0;
    for (size_t k = 0; k < 4; k++) {
        char h = text[i + k];
        int v = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10
              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
        if (v < 0) return -1;
        cp = cp * 16 + v;
    }
    return cp;
}

// Value of the JSON string that starts right after `marker` (e.g. "\"cwd\":\""), unescaped.
// Returns empty when the marker is absent. Never throws.
static std::string JsonString(const std::string& text, const char* marker, bool last = false) {
    size_t i = last ? text.rfind(marker) : text.find(marker);
    if (i == std::string::npos) return {};
    std::string out;
    for (i += strlen(marker); i < text.size() && text[i] != '"'; i++) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        char e = text[++i];
        if (e == 'u') {
            int cp = Hex4(text, i + 1);
            if (cp < 0) {
                out += '\xEF'; out += '\xBF'; out += '\xBD';  // U+FFFD
                continue;
            }
            i += 4;
            // Characters outside the BMP (emoji) come as a \uD8xx\uDCxx surrogate pair.
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < text.size() && text[i + 1] == '\\' &&
                text[i + 2] == 'u') {
                int lo = Hex4(text, i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // lone surrogate
            AppendUtf8(out, (unsigned)cp);
        } else if (e == 'n') {
            out += '\n';
        } else if (e == 't') {
            out += '\t';
        } else if (e == 'r') {
            out += '\r';
        } else if (e == 'b') {
            out += '\b';
        } else if (e == 'f') {
            out += '\f';
        } else {
            out += e;  // \" \\ \/
        }
    }
    return out;
}

// The first or last 512 KB of a transcript. The cwd is in the first lines; the AI title is
// rewritten during the session, so its newest value is near the end. Some lines are huge hook
// attachments, so the read is capped.
static std::string ReadChunk(const std::wstring& path, bool tail) {
    constexpr DWORD kBytes = 512 * 1024;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (tail && GetFileSizeEx(f, &size) && size.QuadPart > kBytes) {
        LARGE_INTEGER pos{};
        pos.QuadPart = size.QuadPart - kBytes;
        SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    }
    std::string buf(kBytes, '\0');
    DWORD read = 0;
    if (!ReadFile(f, buf.data(), kBytes, &read, nullptr)) read = 0;
    CloseHandle(f);
    buf.resize(read);
    return buf;
}

// Trailing separators are ignored; the comparison is case-insensitive like Windows paths.
static bool SameFolder(const std::wstring& a, const std::wstring& b) {
    auto trim = [](std::wstring s) {
        while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        return s;
    };
    return _wcsicmp(trim(a).c_str(), trim(b).c_str()) == 0;
}

static std::wstring FormatTime(FILETIME utc) {
    FILETIME local{};
    FileTimeToLocalFileTime(&utc, &local);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[32];
    swprintf_s(buf, L"%02d.%02d %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
    return buf;
}

// The most recent sessions whose transcript was started in `folder`, newest first.
static std::vector<Session> ScanSessions(const std::wstring& folder) {
    std::vector<Session> out;
    std::wstring base = EnvPath(L"USERPROFILE", L"\\.claude\\projects\\");
    if (base.empty()) return out;
    base += EncodeProjectDir(folder) + L"\\";

    struct Candidate {
        std::wstring id;
        FILETIME mtime;
    };
    std::vector<Candidate> files;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((base + L"*.jsonl").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;  // matches *.jsonl, so it ends with ".jsonl"
        std::wstring id = name.substr(0, name.size() - 6);
        // The id goes into a command line and a URL later, so only a plain UUID is accepted.
        if (IsUuid(id)) files.push_back({id, fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(files.begin(), files.end(), [](const Candidate& a, const Candidate& b) {
        return CompareFileTime(&a.mtime, &b.mtime) > 0;
    });
    for (const Candidate& c : files) {
        if (out.size() >= kMaxSessions) break;
        std::wstring path = base + c.id + L".jsonl";
        std::string head = ReadChunk(path, false);
        if (!SameFolder(Utf8ToWide(JsonString(head, "\"cwd\":\"")), folder)) continue;
        // Sessions started by Claude itself (coordinator relays, scheduled tasks) mark their first
        // prompt with promptSource "system"; typed ones say "typed", "sdk" or nothing.
        if (JsonString(head, "\"promptSource\":\"") == "system") continue;
        // Like Claude Code's own /resume list, a name given with /rename or `claude -n`
        // (customTitle) wins over the generated aiTitle. Both are re-appended, the newest counts.
        std::string tail = ReadChunk(path, true);
        std::string title8 = JsonString(tail, "\"customTitle\":\"", true);
        if (title8.empty()) title8 = JsonString(head, "\"customTitle\":\"", true);
        if (title8.empty()) title8 = JsonString(tail, "\"aiTitle\":\"", true);
        if (title8.empty()) title8 = JsonString(head, "\"aiTitle\":\"", true);
        std::wstring title = Utf8ToWide(title8);
        std::wstring label = FormatTime(c.mtime);
        if (title.size() > 60) title.resize(IS_HIGH_SURROGATE(title[59]) ? 59 : 60);  // keep pairs whole
        if (!title.empty()) label = title + L" - " + label;
        out.push_back({c.id, label});
    }
    return out;
}

// --- Windows Terminal's default profile --------------------------------------

struct Profile {
    std::wstring id;           // defaultProfile value from settings.json (GUID or name)
    std::wstring commandline;  // the profile's commandline; empty for dynamic profiles
    std::wstring source;       // dynamic profile source (e.g. Windows.Terminal.PowershellCore)
};

static bool ReadUtf8File(const std::wstring& path, std::wstring& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 16 * 1024 * 1024;
    std::string bytes;
    if (ok) {
        bytes.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ok = ReadFile(f, bytes.data(), (DWORD)bytes.size(), &read, nullptr) && read == bytes.size();
    }
    CloseHandle(f);
    if (!ok) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    out.assign(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), n);
    if (!out.empty() && out[0] == 0xFEFF) out.erase(0, 1);
    return true;
}

// settings.json is JSONC: drop comments and trailing commas so JsonObject::Parse accepts it.
static std::wstring StripJsonc(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    bool inString = false;
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (inString) {
            o += c;
            if (c == L'\\' && i + 1 < s.size()) o += s[++i];
            else if (c == L'"') inString = false;
            continue;
        }
        if (c == L'"') {
            inString = true;
        } else if (c == L'/' && i + 1 < s.size() && s[i + 1] == L'/') {
            while (i < s.size() && s[i] != L'\n') i++;
            o += L'\n';
            continue;
        } else if (c == L'/' && i + 1 < s.size() && s[i + 1] == L'*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == L'*' && s[i + 1] == L'/')) i++;
            i++;
            continue;
        } else if (c == L'}' || c == L']') {
            size_t k = o.find_last_not_of(L" \t\r\n");
            if (k != std::wstring::npos && o[k] == L',') o.erase(k, 1);
        }
        o += c;
    }
    return o;
}

static bool DefaultProfile(Profile& p) {
    const std::wstring candidates[] = {
        EnvPath(L"LOCALAPPDATA", L"\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json"),
        EnvPath(L"LOCALAPPDATA", L"\\Packages\\Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe\\LocalState\\settings.json"),
        EnvPath(L"LOCALAPPDATA", L"\\Microsoft\\Windows Terminal\\settings.json"),
    };
    for (const auto& path : candidates) {
        std::wstring text;
        if (path.empty() || !ReadUtf8File(path, text)) continue;
        try {
            using namespace winrt::Windows::Data::Json;
            JsonObject root = JsonObject::Parse(StripJsonc(text));
            std::wstring def(root.GetNamedString(L"defaultProfile", L""));
            if (def.empty()) return false;
            JsonValue profiles = root.GetNamedValue(L"profiles", JsonValue::CreateNullValue());
            JsonArray list = nullptr;
            if (profiles.ValueType() == JsonValueType::Object) {
                JsonObject obj = profiles.GetObject();
                if (obj.HasKey(L"list")) list = obj.GetNamedArray(L"list");
            } else if (profiles.ValueType() == JsonValueType::Array) {
                list = profiles.GetArray();
            }
            if (!list) return false;
            for (const IJsonValue& v : list) {
                if (v.ValueType() != JsonValueType::Object) continue;
                JsonObject prof = v.GetObject();
                std::wstring guid(prof.GetNamedString(L"guid", L""));
                std::wstring name(prof.GetNamedString(L"name", L""));
                if (_wcsicmp(guid.c_str(), def.c_str()) != 0 && _wcsicmp(name.c_str(), def.c_str()) != 0) continue;
                p.id = def;
                p.commandline = prof.GetNamedString(L"commandline", L"");
                p.source = prof.GetNamedString(L"source", L"");
                return true;
            }
        } catch (...) {
            // Broken or unexpected settings file: fall back to the default behaviour.
        }
        return false;
    }
    return false;
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

// Does the space-separated (lowercased) command line contain one of these tokens?
static bool HasToken(const std::wstring& lower, std::initializer_list<const wchar_t*> tokens) {
    size_t i = 0;
    while (i < lower.size()) {
        size_t end = lower.find(L' ', i);
        if (end == std::wstring::npos) end = lower.size();
        std::wstring tok = lower.substr(i, end - i);
        for (const wchar_t* t : tokens)
            if (tok == t) return true;
        i = end + 1;
    }
    return false;
}

// Command line that starts claude (with `args`, e.g. " --resume <id>") inside the profile's shell;
// empty for an unknown shell.
static std::wstring ShellWithClaude(const Profile& p, const std::wstring& claude, const std::wstring& args) {
    std::wstring cmd = p.commandline;
    if (cmd.empty() && p.source == L"Windows.Terminal.PowershellCore") {
        wchar_t buf[MAX_PATH];
        DWORD n = SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr);
        if (n > 0 && n < MAX_PATH) cmd = L"\"" + std::wstring(buf) + L"\"";
    }
    if (cmd.empty()) return L"";
    wchar_t expanded[4096];
    DWORD n = ExpandEnvironmentStringsW(cmd.c_str(), expanded, ARRAYSIZE(expanded));
    if (n == 0 || n > ARRAYSIZE(expanded)) return L"";
    cmd = expanded;

    // First token: the shell's exe.
    std::wstring exe;
    if (cmd[0] == L'"') {
        size_t end = cmd.find(L'"', 1);
        exe = cmd.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    } else {
        exe = cmd.substr(0, cmd.find(L' '));
    }
    std::wstring base = Lower(exe.substr(exe.find_last_of(L"\\/") + 1));
    std::wstring lower = Lower(cmd);

    if (base == L"pwsh.exe" || base == L"pwsh" || base == L"powershell.exe" || base == L"powershell") {
        if (HasToken(lower, {L"-c", L"-command", L"-cwa", L"-commandwithargs", L"-f", L"-file", L"-e", L"-ec",
                             L"-encodedcommand"}))
            return L"";  // the profile already runs a command/file; don't add to it
        std::wstring quoted;
        for (wchar_t c : claude) {
            if (c == L'\'') quoted += L'\'';
            quoted += c;
        }
        return cmd + L" -NoExit -Command \"& '" + quoted + L"'" + args + L"\"";
    }
    if (base == L"cmd.exe" || base == L"cmd") {
        if (HasToken(lower, {L"/c", L"/k"})) return L"";
        return cmd + L" /k \"" + claude + L"\"" + args;
    }
    return L"";
}

// In a wt command line ';' separates tabs/commands; a literal ';' needs '\;'.
static std::wstring EscapeWt(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L';') o += L'\\';
        o += c;
    }
    return o;
}

// Opens claude in `folder`; a non-empty resumeId resumes that session instead of starting a new one.
static HRESULT LaunchTerminal(const std::wstring& folder, const std::wstring& resumeId) {
    std::wstring claude = ClaudeCliPath();
    if (claude.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    std::wstring claudeArgs = resumeId.empty() ? std::wstring() : L" --resume " + resumeId;

    std::wstring wt = EnvPath(L"LOCALAPPDATA", L"\\Microsoft\\WindowsApps\\wt.exe");
    if (!wt.empty() && FileExists(wt)) {
        // A trailing backslash would escape the closing quote ("C:\" -> "C:\.").
        std::wstring dir = folder;
        if (!dir.empty() && dir.back() == L'\\') dir += L'.';

        // Open with the default profile (icon, colours and title come from it) and run claude
        // inside its shell, so the shell profile loads and the shell stays when claude exits.
        Profile profile;
        std::wstring args;
        std::wstring inner;
        if (DefaultProfile(profile)) {
            args = L"-p \"" + profile.id + L"\" ";
            inner = ShellWithClaude(profile, claude, claudeArgs);
        }
        if (inner.empty()) inner = L"\"" + claude + L"\"" + claudeArgs;
        args += L"-d \"" + EscapeWt(dir) + L"\" " + EscapeWt(inner);
        HINSTANCE h = ShellExecuteW(nullptr, L"open", wt.c_str(), args.c_str(), folder.c_str(), SW_SHOWNORMAL);
        if ((INT_PTR)h > 32) return S_OK;
    }
    // No Windows Terminal: run claude directly in the default console.
    HINSTANCE h = ShellExecuteW(nullptr, L"open", claude.c_str(), nullptr, folder.c_str(), SW_SHOWNORMAL);
    return (INT_PTR)h > 32 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

static HRESULT OpenUrl(const std::wstring& url) {
    HINSTANCE h = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)h > 32 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

static HRESULT LaunchDesktop(const std::wstring& folder) {
    return OpenUrl(L"claude://code/new?folder=" + UriEncode(folder));
}

// claude://resume imports a CLI transcript by its UUID and opens it in the desktop app.
static HRESULT LaunchDesktopResume(const std::wstring& sessionId) {
    return OpenUrl(L"claude://resume?session=" + sessionId);
}

static HRESULT Launch(Kind kind, const std::wstring& folder, const std::wstring& resumeId) {
    if (kind == Kind::Terminal) return LaunchTerminal(folder, resumeId);
    return resumeId.empty() ? LaunchDesktop(folder) : LaunchDesktopResume(resumeId);
}

// Root: the top-level entry. Its submenu holds New, a separator and one Resume per session.
enum class Role { Root, New, Separator, Resume };

// Enumerator of a submenu. It owns one reference to each child.
class EnumCommands final : public IEnumExplorerCommand {
public:
    explicit EnumCommands(std::vector<IExplorerCommand*> items) : items_(std::move(items)) {
        InterlockedIncrement(&g_objects);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumExplorerCommand) {
            *ppv = static_cast<IEnumExplorerCommand*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }

    // IEnumExplorerCommand
    IFACEMETHODIMP Next(ULONG count, IExplorerCommand** out, ULONG* fetched) override {
        ULONG n = 0;
        while (n < count && pos_ < items_.size()) {
            items_[pos_]->AddRef();
            out[n++] = items_[pos_++];
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        pos_ = (std::min)(items_.size(), pos_ + (size_t)count);
        return S_OK;
    }
    IFACEMETHODIMP Reset() override {
        pos_ = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumExplorerCommand** e) override {
        if (e) *e = nullptr;
        return E_NOTIMPL;
    }

private:
    ~EnumCommands() {
        for (IExplorerCommand* c : items_) c->Release();
        InterlockedDecrement(&g_objects);
    }
    LONG refs_ = 1;
    std::vector<IExplorerCommand*> items_;
    size_t pos_ = 0;
};

class Command final : public IExplorerCommand, public IObjectWithSite {
public:
    // `folder` is the clicked folder for a Resume child (the session's own folder).
    Command(Kind kind, Role role, const Session& session = Session(), const std::wstring& folder = L"")
        : kind_(kind), role_(role), session_(session), folder_(folder) {
        InterlockedIncrement(&g_objects);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExplorerCommand) {
            *ppv = static_cast<IExplorerCommand*>(this);
        } else if (riid == IID_IObjectWithSite) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }

    // IExplorerCommand
    // Title follows the Windows display language: Turkish, otherwise English.
    IFACEMETHODIMP GetTitle(IShellItemArray*, PWSTR* name) override {
        *name = nullptr;
        bool tr = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH;
        switch (role_) {
        case Role::Separator:
            return SHStrDupW(L"", name);
        case Role::New:
            return SHStrDupW(tr ? L"Yeni oturum" : L"New session", name);
        case Role::Resume:
            return SHStrDupW(session_.label.c_str(), name);
        case Role::Root:
            break;
        }
        const wchar_t* title = kind_ == Kind::Terminal
            ? (tr ? L"Claude Code'da Aç" : L"Open in Claude Code")
            : (tr ? L"Claude Masaüstünde Aç" : L"Open in Claude Desktop");
        return SHStrDupW(title, name);
    }
    // Prefers icon.ico next to the DLL (install.ps1 writes both with the same padding, since
    // the two exes' own icons differ in size); falls back to the exe's first icon.
    IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override {
        if (role_ == Role::Separator) {
            *icon = nullptr;
            return E_NOTIMPL;
        }
        wchar_t self[MAX_PATH];
        DWORD len = GetModuleFileNameW(g_module, self, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring ico = self;
            ico.resize(ico.find_last_of(L'\\') + 1);
            ico += L"icon.ico";
            if (FileExists(ico)) return SHStrDupW(ico.c_str(), icon);
        }
        std::wstring exe = kind_ == Kind::Terminal ? ClaudeCliPath() : DesktopExePath();
        if (exe.empty()) {
            *icon = nullptr;
            return E_NOTIMPL;
        }
        return SHStrDupW((exe + L",0").c_str(), icon);
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* tip) override {
        *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override {
        if (role_ != Role::Root) return E_NOTIMPL;
        *guid = kind_ == Kind::Terminal ? CLSID_ClaudeTerminal : CLSID_ClaudeDesktop;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        // The submenu is built from the clicked folder, and GetFlags/EnumSubCommands get no items.
        if (role_ == Role::Root) {
            std::wstring folder;
            if (SUCCEEDED(TargetFolder(items, site_, folder)) && folder != folder_) {
                folder_ = folder;
                scanned_ = false;
                sessions_.clear();
            }
        }
        bool available = kind_ == Kind::Terminal ? !ClaudeCliPath().empty() : !DesktopExePath().empty();
        *state = available ? ECS_ENABLED : ECS_HIDDEN;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        if (role_ == Role::Separator) return E_NOTIMPL;
        if (role_ == Role::Resume) return Launch(kind_, folder_, session_.id);
        std::wstring folder;
        HRESULT hr = TargetFolder(items, site_, folder);
        if (FAILED(hr)) return hr;
        return Launch(kind_, folder, L"");
    }
    // The root has a submenu only when the folder has sessions; otherwise it stays a plain entry.
    // (ECF_HASSPLITBUTTON was tried: the new Windows 11 menu never invokes the root then.)
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_DEFAULT;
        if (role_ == Role::Separator) *flags = ECF_ISSEPARATOR;
        else if (role_ == Role::Root && HasSessions()) *flags = ECF_HASSUBCOMMANDS;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** e) override {
        *e = nullptr;
        if (role_ != Role::Root || !HasSessions()) return E_NOTIMPL;
        std::vector<IExplorerCommand*> children;
        children.push_back(NewChild(Role::New, Session()));
        children.push_back(NewChild(Role::Separator, Session()));
        for (const Session& s : sessions_) children.push_back(NewChild(Role::Resume, s));
        *e = new (std::nothrow) EnumCommands(std::move(children));
        return *e ? S_OK : E_OUTOFMEMORY;
    }

    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown* site) override {
        if (site) site->AddRef();
        if (site_) site_->Release();
        site_ = site;
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override {
        if (!site_) {
            *ppv = nullptr;
            return E_FAIL;
        }
        return site_->QueryInterface(riid, ppv);
    }

private:
    ~Command() {
        if (site_) site_->Release();
        InterlockedDecrement(&g_objects);
    }

    // Sessions are read once per folder, the first time the root is asked for its flags.
    bool HasSessions() {
        if (folder_.empty()) return false;
        if (!scanned_) {
            sessions_ = ScanSessions(folder_);
            scanned_ = true;
        }
        return !sessions_.empty();
    }

    // Children get the site so that New resolves the clicked folder when it is invoked.
    IExplorerCommand* NewChild(Role role, const Session& session) {
        Command* child = new (std::nothrow) Command(kind_, role, session, folder_);
        if (!child) return nullptr;
        child->SetSite(site_);
        return child;
    }

    LONG refs_ = 1;
    Kind kind_;
    Role role_;
    Session session_;
    std::wstring folder_;  // Root: the clicked folder; Resume: the session's folder
    std::vector<Session> sessions_;
    bool scanned_ = false;
    IUnknown* site_ = nullptr;
};

class Factory final : public IClassFactory {
public:
    explicit Factory(Kind kind) : kind_(kind) {}
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Command* cmd = new (std::nothrow) Command(kind_, Role::Root);
        if (!cmd) return E_OUTOFMEMORY;
        HRESULT hr = cmd->QueryInterface(riid, ppv);
        cmd->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_locks);
        else InterlockedDecrement(&g_locks);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    Kind kind_;
};

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    *ppv = nullptr;
    Kind kind;
    if (clsid == CLSID_ClaudeTerminal) kind = Kind::Terminal;
    else if (clsid == CLSID_ClaudeDesktop) kind = Kind::Desktop;
    else return CLASS_E_CLASSNOTAVAILABLE;
    Factory* f = new (std::nothrow) Factory(kind);
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
