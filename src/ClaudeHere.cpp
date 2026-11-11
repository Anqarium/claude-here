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

// Command line that starts claude inside the profile's shell; empty for an unknown shell.
static std::wstring ShellWithClaude(const Profile& p, const std::wstring& claude) {
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
        return cmd + L" -NoExit -Command \"& '" + quoted + L"'\"";
    }
    if (base == L"cmd.exe" || base == L"cmd") {
        if (HasToken(lower, {L"/c", L"/k"})) return L"";
        return cmd + L" /k \"" + claude + L"\"";
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

static HRESULT LaunchTerminal(const std::wstring& folder) {
    std::wstring claude = ClaudeCliPath();
    if (claude.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

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
            inner = ShellWithClaude(profile, claude);
        }
        if (inner.empty()) inner = L"\"" + claude + L"\"";
        args += L"-d \"" + EscapeWt(dir) + L"\" " + EscapeWt(inner);
        HINSTANCE h = ShellExecuteW(nullptr, L"open", wt.c_str(), args.c_str(), folder.c_str(), SW_SHOWNORMAL);
        if ((INT_PTR)h > 32) return S_OK;
    }
    // No Windows Terminal: run claude directly in the default console.
    HINSTANCE h = ShellExecuteW(nullptr, L"open", claude.c_str(), nullptr, folder.c_str(), SW_SHOWNORMAL);
    return (INT_PTR)h > 32 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

static HRESULT LaunchDesktop(const std::wstring& folder) {
    std::wstring url = L"claude://code/new?folder=" + UriEncode(folder);
    HINSTANCE h = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)h > 32 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

class Command final : public IExplorerCommand, public IObjectWithSite {
public:
    explicit Command(Kind kind) : kind_(kind) { InterlockedIncrement(&g_objects); }

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
        bool tr = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH;
        const wchar_t* title = kind_ == Kind::Terminal
            ? (tr ? L"Claude Code'da Aç" : L"Open in Claude Code")
            : (tr ? L"Claude Masaüstünde Aç" : L"Open in Claude Desktop");
        return SHStrDupW(title, name);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override {
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
        *guid = kind_ == Kind::Terminal ? CLSID_ClaudeTerminal : CLSID_ClaudeDesktop;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* state) override {
        bool available = kind_ == Kind::Terminal ? !ClaudeCliPath().empty() : !DesktopExePath().empty();
        *state = available ? ECS_ENABLED : ECS_HIDDEN;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        std::wstring folder;
        HRESULT hr = TargetFolder(items, site_, folder);
        if (FAILED(hr)) return hr;
        return kind_ == Kind::Terminal ? LaunchTerminal(folder) : LaunchDesktop(folder);
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** e) override {
        *e = nullptr;
        return E_NOTIMPL;
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
    LONG refs_ = 1;
    Kind kind_;
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
        Command* cmd = new (std::nothrow) Command(kind_);
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
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
