# claude-here

Windows 11'in yeni (kısa) sağ tık menüsüne iki girdi ekler:

- **Claude Code'da Aç** — Windows Terminal'de, tıklanan klasörde `claude` (Claude Code CLI) açar.
  Windows Terminal yoksa `claude.exe` varsayılan konsolda açılır.
- **Claude Masaüstünde Aç** — Claude masaüstü uygulamasında o klasörle yeni bir Claude Code
  oturumu açar (`claude://code/new?folder=...`). Uygulama kurulu değilse girdi gizlenir.

Her ikisi de klasöre sağ tıklayınca ve klasörün içinde boşluğa sağ tıklayınca çıkar.

## Nasıl çalışır

`IExplorerCommand` uygulayan küçük bir C++ COM DLL'i (`src/ClaudeHere.cpp`), her komut için
ayrı bir *sparse* paketle kaydedilir (`package/AppxManifest.template.xml`). Windows Terminal'in
"Terminalde aç" girdisi de aynı yapıyı kullanır. DLL Explorer'ın içinde değil, `dllhost.exe`
surrogate sürecinde çalışır. İki ayrı paket olduğu için Windows ikisini tek bir alt menüde
toplamaz ve **Ayarlar → Kişiselleştirme → … → App extensions** sayfasında her biri ayrı
açılıp kapatılabilir.

## Gereksinimler

- Windows 11, **Developer Mode açık** (paket imzasız kaydedilir).
- Visual Studio 2022 C++ araçları ve Windows 11 SDK
  (`winget install Microsoft.WindowsSDK.10.0.26100`).
- Claude Code CLI (`%USERPROFILE%\.local\bin\claude.exe` veya PATH'te).

## Kurulum

```powershell
.\build.ps1      # build\ClaudeHere.dll ve build\ClaudeHereStub.exe
.\install.ps1    # %LOCALAPPDATA%\ClaudeHere altına kopyalar ve paketleri kaydeder
```

Güncellemek için ikisini yeniden çalıştırmak yeterli. Kaldırmak için:

```powershell
.\uninstall.ps1
```
