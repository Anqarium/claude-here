# CLAUDE.md

Windows 11 sağ tık menüsüne "Claude Code'da Aç" ve "Claude Masaüstünde Aç" ekleyen
`IExplorerCommand` shell uzantısı. Ayrıntılar `README.md`'de.

## Komutlar

```powershell
.\build.ps1      # MSVC (vcvars64) ile build\ altına derler
.\install.ps1    # %LOCALAPPDATA%\ClaudeHere'ye kopyalar, iki sparse paketi kaydeder
.\uninstall.ps1  # paketleri ve kurulum klasörünü kaldırır
```

## Notlar

- Paketler imzasız, `Add-AppxPackage -Register -ExternalLocation` ile kaydedilir; Developer
  Mode gerekir. Kapalıysa makeappx + self-signed sertifika (TrustedPeople, yönetici) gerekir.
- Repodan değil `%LOCALAPPDATA%\ClaudeHere`'den kaydedilir: repo OneDrive altında ve yüklü DLL
  kilitlenir. `install.ps1` önce DLL'i tutan `dllhost` süreçlerini kapatır.
- Komutlar bilerek iki ayrı pakettir (`ClaudeHere.Terminal`, `ClaudeHere.Desktop`): Windows
  bir paketteki birden çok verb'ü alt menüde toplayabilir, terminal girdisi üst düzeyde kalmalı.
- CLSID'ler `src/ClaudeHere.cpp` ve `install.ps1`'de; ikisi birlikte değişmeli.
- Masaüstü derin bağlantısı `claude://code/new?folder=<encodeURIComponent>`, Claude masaüstü
  uygulamasının kendi `ccd` takma adının ürettiği biçim (app.asar'dan okundu).
- Dil: menü başlığı `GetUserDefaultUILanguage` (Windows arayüz dili, Explorer'ın kullandığı) ile
  seçilir, paket adı kurulumda `Get-UICulture` ile. Windows Terminal ise dil tercih listesini
  izler; bu makinede arayüz `en-US`, liste `tr` önde olduğu için "Terminalde Aç" Türkçe görünür.
- Masaüstü `folder` bağlantısı (`src=external`) her seferinde onay ister; uygulamanın güvenlik
  kararı, aşılmaz. Masaüstünün güven listesi (`localAgentModeTrustedFolders`) CLI'nin
  `~/.claude.json` listesinden ayrı.
- Menünün kendisi otomatik test edilemiyor; COM tarafı `CoCreateInstance` + `GetTitle`/`GetState`/
  `Invoke` çağıran küçük bir probe ile doğrulandı (2026-11-12).
