// Windows 11 sağ tık menüsüne iki komut ekleyen IExplorerCommand sunucusu:
//   - Terminal: Windows Terminal'de, seçilen klasörde `claude` (Claude Code CLI) açar.
//   - Masaüstü: Claude masaüstü uygulamasında o klasörle yeni bir Claude Code oturumu açar.
// Her komut ayrı bir sparse paketle kaydedilir (package/*.xml), DLL dllhost.exe içinde çalışır.

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

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

// {0F55B8AB-4947-4EAB-B1F7-4E8C0DB27AC9}
static const CLSID CLSID_ClaudeTerminal = {0x0f55b8ab, 0x4947, 0x4eab, {0xb1, 0xf7, 0x4e, 0x8c, 0x0d, 0xb2, 0x7a, 0xc9}};
// {59847620-2A77-4B45-AD83-EF512E443B3B}
static const CLSID CLSID_ClaudeDesktop = {0x59847620, 0x2a77, 0x4b45, {0xad, 0x83, 0xef, 0x51, 0x2e, 0x44, 0x3b, 0x3b}};

// Claude masaüstü uygulamasının paket ailesi (yayıncı karması sabittir, sürümle değişmez).
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

// Claude Code CLI: önce yerel kurulumun varsayılan yeri, sonra PATH.
static std::wstring ClaudeCliPath() {
    std::wstring p = EnvPath(L"USERPROFILE", L"\\.local\\bin\\claude.exe");
    if (!p.empty() && FileExists(p)) return p;
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, L"claude.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) return buf;
    return L"";
}

// Claude masaüstü uygulamasının exe yolu; kurulu değilse boş.
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

// Seçilen klasör; klasör arka planına tıklanınca dizi boş gelebilir, o zaman
// pencerenin kendi klasörü site → IFolderView üzerinden alınır.
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

// encodeURIComponent eşdeğeri (UTF-8).
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

static HRESULT LaunchTerminal(const std::wstring& folder) {
    std::wstring claude = ClaudeCliPath();
    if (claude.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    std::wstring wt = EnvPath(L"LOCALAPPDATA", L"\\Microsoft\\WindowsApps\\wt.exe");
    if (!wt.empty() && FileExists(wt)) {
        // Sondaki ters bölü kapanış tırnağını kaçırır ("C:\" → "C:\."), wt ';' ile komut ayırır.
        std::wstring dir = folder;
        if (!dir.empty() && dir.back() == L'\\') dir += L'.';
        std::wstring escaped;
        for (wchar_t c : dir) {
            if (c == L';') escaped += L'\\';
            escaped += c;
        }
        std::wstring args = L"-d \"" + escaped + L"\" \"" + claude + L"\"";
        HINSTANCE h = ShellExecuteW(nullptr, L"open", wt.c_str(), args.c_str(), folder.c_str(), SW_SHOWNORMAL);
        if ((INT_PTR)h > 32) return S_OK;
    }
    // Windows Terminal yoksa: varsayılan konsolda doğrudan claude.
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
    // Başlık Windows arayüz diline göre: Türkçe, değilse İngilizce.
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
