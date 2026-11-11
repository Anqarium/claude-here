// A sparse package manifest needs an <Application> with an exe; the menu commands
// don't use it. If started, it exits without doing anything.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return 0; }
