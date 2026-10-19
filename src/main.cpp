#include "cli.h"
#include "ui.h"

#include <objbase.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool cli = false;
    for (auto& a : args)
        if (a == L"--scan" || a == L"--clean" || a == L"--help" || a == L"-h" || a == L"--version") cli = true;
    int code = cli ? roomy::runCli(args) : roomy::runUi(inst, show, args);
    CoUninitialize();
    return code;
}
