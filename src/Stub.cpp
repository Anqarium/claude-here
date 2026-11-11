// Sparse paket manifesti bir <Application> ve onun exe'sini ister; menü komutları
// bu exe'yi kullanmaz. Başlatılırsa hiçbir şey yapmadan çıkar.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return 0; }
