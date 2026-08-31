#include "app.hpp"

#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(comResult)) return 1;
    const HRESULT mediaResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mediaResult)) {
        CoUninitialize();
        return 2;
    }

    int result = 3;
    {
        fillema::App app(instance);
        if (app.initialize(showCommand)) result = app.run();
    }

    MFShutdown();
    CoUninitialize();
    return result;
}
