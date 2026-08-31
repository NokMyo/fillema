#include "app.hpp"

#include "resource.h"
#include "win_process.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <system_error>

namespace fillema {
namespace {

constexpr wchar_t kMainClass[] = L"FebiusFillemaMain";
constexpr wchar_t kTimelineClass[] = L"FebiusFillemaTimeline";
constexpr wchar_t kInspectorClass[] = L"FebiusFillemaInspector";
constexpr wchar_t kStillClass[] = L"FebiusFillemaStill";
constexpr wchar_t kSplashClass[] = L"FebiusFillemaSplash";
constexpr wchar_t kCommandClass[] = L"FebiusFillemaCommand";

constexpr COLORREF kWindow = RGB(16, 17, 20);
constexpr COLORREF kPanel = RGB(23, 24, 28);
constexpr COLORREF kPanelRaised = RGB(31, 32, 37);
constexpr COLORREF kInput = RGB(39, 40, 46);
constexpr COLORREF kBorder = RGB(54, 55, 62);
constexpr COLORREF kText = RGB(239, 239, 242);
constexpr COLORREF kMuted = RGB(158, 159, 168);
constexpr COLORREF kAccent = RGB(235, 108, 58);
constexpr COLORREF kAccentSoft = RGB(131, 67, 49);
constexpr COLORREF kTimelineClip = RGB(58, 61, 72);
constexpr COLORREF kTimelineAudio = RGB(42, 61, 58);

constexpr int kToolbarHeight = 52;
constexpr int kStatusHeight = 26;
constexpr int kMediaWidth = 244;
constexpr int kInspectorWidth = 350;
constexpr int kTransportHeight = 38;

struct ProbeCompletion {
    std::int64_t mediaId = 0;
    ProbeInfo info;
    std::wstring error;
};

struct ProxyCompletion {
    std::int64_t mediaId = 0;
    std::filesystem::path path;
    bool success = false;
};

struct PreviewCompletion {
    std::int64_t clipId = 0;
    std::uint64_t generation = 0;
    std::filesystem::path path;
    bool success = false;
    std::wstring error;
};

struct ExportCompletion {
    std::filesystem::path output;
    bool success = false;
    std::wstring error;
};

App* WindowApp(HWND window) {
    return reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

std::wstring FileName(const std::filesystem::path& path) {
    return path.filename().wstring();
}

std::filesystem::path AddExtensionIfMissing(std::filesystem::path path, std::wstring_view extension) {
    if (path.extension().empty()) path += extension;
    return path;
}

bool IsEditLike(HWND window) {
    if (!window) return false;
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return _wcsicmp(className, L"Edit") == 0 || _wcsicmp(className, L"ComboBox") == 0;
}

std::wstring ErrorFromException(const std::exception& exception) {
    return Utf8ToWide(exception.what());
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(std::max(0, length)));
    return text;
}

void SetControlText(HWND window, const std::wstring& text) {
    if (window) SetWindowTextW(window, text.c_str());
}

} // namespace

App::App(HINSTANCE instance) : instance_(instance) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    logicalProcessors_ = std::max(1UL, system.dwNumberOfProcessors);
    MEMORYSTATUSEX memory{sizeof(MEMORYSTATUSEX)};
    if (GlobalMemoryStatusEx(&memory)) physicalMemoryMb_ = memory.ullTotalPhys / (1024ULL * 1024ULL);
    adaptiveProfile_ = ChooseAdaptiveProfile(PerformanceMode::Automatic, logicalProcessors_, physicalMemoryMb_);

    ffmpegPath_ = FindBundledTool(L"ffmpeg.exe");
    ffprobePath_ = FindBundledTool(L"ffprobe.exe");
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
        cacheDirectory_ = std::filesystem::path(localAppData) / L"Febius" / L"Fillema" / L"Cache";
        CoTaskMemFree(localAppData);
    } else {
        cacheDirectory_ = std::filesystem::temp_directory_path() / L"Febius-Fillema";
    }
    std::error_code error;
    std::filesystem::create_directories(cacheDirectory_, error);

    commands_ = {
        {L"미디어 가져오기  ·  Import media", ID_FILE_IMPORT},
        {L"프로젝트 열기  ·  Open project", ID_FILE_OPEN},
        {L"프로젝트 저장  ·  Save project", ID_FILE_SAVE},
        {L"현재 위치 분할  ·  Split clip", ID_EDIT_SPLIT},
        {L"리플 삭제  ·  Ripple delete", ID_EDIT_RIPPLE_DELETE},
        {L"색보정 열기  ·  Color grading", ID_TAB_COLOR_BASIC},
        {L"노출 조정  ·  Exposure", ID_PROP_EXPOSURE},
        {L"정밀 색감 열기  ·  Curves and wheels", ID_TAB_COLOR_PRECISE},
        {L"텍스트 추가  ·  Add title", ID_TEXT_ADD},
        {L"1080p 출력 설정", 10001},
        {L"4K 출력 설정", 10002},
        {L"영상 출력  ·  Export", ID_FILE_EXPORT},
        {L"전체 화면 미리보기  ·  Fullscreen", ID_VIEW_FULLSCREEN}
    };
}

App::~App() {
    previewWorker_.stop();
    mediaWorker_.stop();
    exportWorker_.stop();
    player_.shutdown();
    clearPreviewBitmap();
    if (uiFont_) DeleteObject(uiFont_);
    if (semiboldFont_) DeleteObject(semiboldFont_);
    if (windowBrush_) DeleteObject(windowBrush_);
    if (panelBrush_) DeleteObject(panelBrush_);
    if (inputBrush_) DeleteObject(inputBrush_);
}

bool App::initialize(int showCommand) {
    windowBrush_ = CreateSolidBrush(kWindow);
    panelBrush_ = CreateSolidBrush(kPanel);
    inputBrush_ = CreateSolidBrush(kInput);
    uiFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, HANGUL_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
    semiboldFont_ = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, HANGUL_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
    if (!registerWindowClasses() || !createMainWindow(showCommand)) return false;
    createMenu();
    createControls();
    createSplash();
    DragAcceptFiles(mainWindow_, TRUE);
    player_.initialize(previewWindow_);
    SetTimer(mainWindow_, TIMER_SPLASH, 420, nullptr);
    SetTimer(mainWindow_, TIMER_PLAYHEAD, 33, nullptr);
    SetTimer(mainWindow_, TIMER_AUTOSAVE, 30'000, nullptr);
    refreshFromProject();
    return true;
}

int App::run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && GetAncestor(message.hwnd, GA_ROOT) == mainWindow_ && handleShortcut(message.wParam)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

bool App::registerWindowClasses() {
    const auto registerClass = [&](const wchar_t* name, WNDPROC proc, HBRUSH background, UINT style = CS_DBLCLKS) {
        WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
        windowClass.style = style;
        windowClass.lpfnWndProc = proc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hIconSm = windowClass.hIcon;
        windowClass.hbrBackground = background;
        windowClass.lpszClassName = name;
        return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    };
    return registerClass(kMainClass, MainWindowProc, windowBrush_, CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW)
        && registerClass(kTimelineClass, TimelineWindowProc, panelBrush_, CS_DBLCLKS)
        && registerClass(kInspectorClass, InspectorWindowProc, panelBrush_, CS_DBLCLKS)
        && registerClass(kStillClass, StillWindowProc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)), CS_DBLCLKS)
        && registerClass(kSplashClass, SplashWindowProc, windowBrush_, CS_DBLCLKS)
        && registerClass(kCommandClass, CommandPanelProc, panelBrush_, CS_DBLCLKS);
}

bool App::createMainWindow(int showCommand) {
    mainWindow_ = CreateWindowExW(0, kMainClass, L"Febius Fillema", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, instance_, this);
    if (!mainWindow_) return false;
    applyDarkMode(mainWindow_);
    ShowWindow(mainWindow_, SW_HIDE);
    UpdateWindow(mainWindow_);
    (void)showCommand;
    return true;
}

void App::createMenu() {
    HMENU menuBar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_FILE_NEW, L"새 프로젝트\tCtrl+N");
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN, L"열기...\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_FILE_SAVE, L"저장\tCtrl+S");
    AppendMenuW(file, MF_STRING, ID_FILE_SAVE_AS, L"다른 이름으로 저장...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_IMPORT, L"미디어 가져오기...\tCtrl+I");
    AppendMenuW(file, MF_STRING, ID_FILE_EXPORT, L"영상 출력...\tCtrl+E");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_EXIT, L"끝내기");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"파일");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, ID_EDIT_UNDO, L"실행 취소\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, ID_EDIT_REDO, L"다시 실행\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, ID_EDIT_SPLIT, L"현재 위치 분할\tS");
    AppendMenuW(edit, MF_STRING, ID_EDIT_RIPPLE_DELETE, L"리플 삭제\tDelete");
    AppendMenuW(edit, MF_STRING, ID_EDIT_DUPLICATE, L"클립 복제\tCtrl+D");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"편집");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, ID_VIEW_COMMAND_BAR, L"명령 검색\tCtrl+P");
    AppendMenuW(view, MF_STRING, ID_VIEW_FULLSCREEN, L"전체 화면 미리보기\tF");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"보기");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"Fillema 정보");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"도움말");
    mainMenu_ = menuBar;
    SetMenu(mainWindow_, menuBar);
}

void App::createControls() {
    const auto button = [&](const wchar_t* text, int id) {
        HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 80, 30, mainWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        setFont(control, id == ID_TOOL_EXPORT);
        applyDarkMode(control);
        toolbarControls_.push_back(control);
        return control;
    };
    HWND brand = CreateWindowExW(0, L"STATIC", L"FILLEMA  0.1", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        0, 0, 160, 34, mainWindow_, nullptr, instance_, nullptr);
    setFont(brand, true);
    toolbarControls_.push_back(brand);
    button(L"가져오기", ID_TOOL_IMPORT);
    button(L"저장", ID_TOOL_SAVE);
    button(L"실행 취소", ID_TOOL_UNDO);
    button(L"다시 실행", ID_TOOL_REDO);
    button(L"분할", ID_TOOL_SPLIT);
    button(L"영상 출력", ID_TOOL_EXPORT);

    HWND performanceLabel = CreateWindowExW(0, L"STATIC", L"성능", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        0, 0, 38, 28, mainWindow_, nullptr, instance_, nullptr);
    setFont(performanceLabel);
    toolbarControls_.push_back(performanceLabel);
    performanceCombo_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 128, 300, mainWindow_, reinterpret_cast<HMENU>(ID_PERFORMANCE_MODE), instance_, nullptr);
    for (const wchar_t* item : {L"자동", L"품질 우선", L"균형", L"속도 우선"}) SendMessageW(performanceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(performanceCombo_, CB_SETCURSEL, 0, 0);
    setFont(performanceCombo_);
    applyDarkMode(performanceCombo_);
    toolbarControls_.push_back(performanceCombo_);

    mediaHeader_ = CreateWindowExW(0, L"STATIC", L"  미디어", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        0, 0, kMediaWidth, 38, mainWindow_, nullptr, instance_, nullptr);
    setFont(mediaHeader_, true);
    mediaList_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
        0, 0, kMediaWidth, 500, mainWindow_, reinterpret_cast<HMENU>(ID_MEDIA_LIST), instance_, nullptr);
    SendMessageW(mediaList_, LB_SETITEMHEIGHT, 0, 62);
    setFont(mediaList_);
    applyDarkMode(mediaList_);

    previewWindow_ = CreateWindowExW(0, kStillClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 500, 300, mainWindow_, nullptr, instance_, this);
    stillWindow_ = CreateWindowExW(0, kStillClass, nullptr, WS_CHILD,
        0, 0, 500, 300, previewWindow_, nullptr, instance_, this);
    playButton_ = CreateWindowExW(0, L"BUTTON", L"▶  재생", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 90, 30, mainWindow_, reinterpret_cast<HMENU>(ID_PLAY_PAUSE), instance_, nullptr);
    setFont(playButton_);
    applyDarkMode(playButton_);
    timeLabel_ = CreateWindowExW(0, L"STATIC", L"00:00.000 / 00:00.000", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        0, 0, 210, 30, mainWindow_, nullptr, instance_, nullptr);
    setFont(timeLabel_);

    timelineWindow_ = CreateWindowExW(0, kTimelineClass, nullptr, WS_CHILD | WS_VISIBLE,
        0, 0, 500, 220, mainWindow_, nullptr, instance_, this);
    inspectorWindow_ = CreateWindowExW(0, kInspectorClass, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, kInspectorWidth, 600, mainWindow_, nullptr, instance_, this);
    statusWindow_ = CreateWindowExW(0, L"STATIC", L"준비", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        0, 0, 500, kStatusHeight, mainWindow_, nullptr, instance_, nullptr);
    setFont(statusWindow_);

    commandPanel_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kCommandClass, nullptr,
        WS_CHILD | WS_BORDER, 0, 0, 540, 290, mainWindow_, nullptr, instance_, this);
    commandEdit_ = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        14, 14, 510, 38, commandPanel_, reinterpret_cast<HMENU>(ID_COMMAND_EDIT), instance_, nullptr);
    commandList_ = CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
        14, 62, 510, 210, commandPanel_, reinterpret_cast<HMENU>(ID_COMMAND_LIST), instance_, nullptr);
    setFont(commandEdit_);
    setFont(commandList_);
    applyDarkMode(commandEdit_);
    applyDarkMode(commandList_);
    SetWindowSubclass(commandEdit_, CommandEditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
    filterCommands();
}

void App::createSplash() {
    const int width = 460;
    const int height = 260;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    splashWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kSplashClass, nullptr, WS_POPUP,
        work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance_, this);
    ShowWindow(splashWindow_, SW_SHOWNOACTIVATE);
    UpdateWindow(splashWindow_);
}

void App::layout(int width, int height) {
    if (!mediaHeader_) return;
    if (fullscreen_) {
        MoveWindow(previewWindow_, 0, 0, width, height, TRUE);
        MoveWindow(stillWindow_, 0, 0, width, height, TRUE);
        player_.updateVideo();
        return;
    }

    int x = 14;
    const int y = 9;
    MoveWindow(toolbarControls_[0], x, y, 154, 34, TRUE); x += 164;
    for (int id : {ID_TOOL_IMPORT, ID_TOOL_SAVE, ID_TOOL_UNDO, ID_TOOL_REDO, ID_TOOL_SPLIT, ID_TOOL_EXPORT}) {
        HWND control = GetDlgItem(mainWindow_, id);
        const int controlWidth = id == ID_TOOL_EXPORT ? 92 : 78;
        MoveWindow(control, x, y + 2, controlWidth, 30, TRUE);
        x += controlWidth + 6;
    }
    HWND performanceLabel = toolbarControls_[7];
    MoveWindow(performanceCombo_, width - 154, y + 2, 136, 300, TRUE);
    MoveWindow(performanceLabel, width - 198, y + 2, 40, 30, TRUE);

    const int contentTop = kToolbarHeight;
    const int contentBottom = std::max(contentTop, height - kStatusHeight);
    const int contentHeight = contentBottom - contentTop;
    const int rightX = std::max(kMediaWidth + 400, width - kInspectorWidth);
    const int centerX = kMediaWidth + 1;
    const int centerWidth = std::max(200, rightX - centerX - 1);
    int timelineHeight = std::clamp(contentHeight / 3, 190, 280);
    int previewHeight = std::max(180, contentHeight - timelineHeight - kTransportHeight);
    if (previewHeight + timelineHeight + kTransportHeight > contentHeight) timelineHeight = std::max(130, contentHeight - previewHeight - kTransportHeight);

    MoveWindow(mediaHeader_, 0, contentTop, kMediaWidth, 38, TRUE);
    MoveWindow(mediaList_, 0, contentTop + 38, kMediaWidth, std::max(0, contentHeight - 38), TRUE);
    MoveWindow(previewWindow_, centerX, contentTop, centerWidth, previewHeight, TRUE);
    MoveWindow(stillWindow_, 0, 0, centerWidth, previewHeight, TRUE);
    MoveWindow(playButton_, centerX + 12, contentTop + previewHeight + 4, 92, 30, TRUE);
    MoveWindow(timeLabel_, centerX + 114, contentTop + previewHeight + 4, 230, 30, TRUE);
    MoveWindow(timelineWindow_, centerX, contentTop + previewHeight + kTransportHeight, centerWidth, timelineHeight, TRUE);
    MoveWindow(inspectorWindow_, rightX, contentTop, width - rightX, contentHeight, TRUE);
    MoveWindow(statusWindow_, 0, contentBottom, width, kStatusHeight, TRUE);
    MoveWindow(commandPanel_, std::max(0, (width - 540) / 2), contentTop + 18, 540, 290, TRUE);
    player_.updateVideo();
}

void App::paintMain(HDC context) {
    RECT client{};
    GetClientRect(mainWindow_, &client);
    FillRect(context, &client, windowBrush_);
    RECT toolbar{0, 0, client.right, kToolbarHeight};
    HBRUSH toolbarBrush = CreateSolidBrush(kPanel);
    FillRect(context, &toolbar, toolbarBrush);
    DeleteObject(toolbarBrush);
    HPEN border = CreatePen(PS_SOLID, 1, kBorder);
    const HGDIOBJ oldPen = SelectObject(context, border);
    MoveToEx(context, 0, kToolbarHeight - 1, nullptr);
    LineTo(context, client.right, kToolbarHeight - 1);
    SelectObject(context, oldPen);
    DeleteObject(border);
}

void App::applyDarkMode(HWND window) {
    if (!window) return;
    BOOL enabled = TRUE;
    DwmSetWindowAttribute(window, 20, &enabled, sizeof(enabled));
    SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
}

void App::setFont(HWND window, bool semibold) {
    if (window) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(semibold ? semiboldFont_ : uiFont_), TRUE);
}

LRESULT CALLBACK App::MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = WindowApp(window);
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app) app->mainWindow_ = window;
    }
    return app ? app->handleMessage(window, message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK App::TimelineWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = WindowApp(window);
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC context = BeginPaint(window, &paint);
        app->paintTimeline(window, context);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(app->mainWindow_);
        const TimelineHit hit = app->hitTimeline(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hit.valid) {
            app->selectTimelineTime(hit.globalTime);
            app->draggingTimeline_ = true;
            app->draggingClipId_ = app->project_.timeline[hit.index].id;
            SetCapture(window);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if ((wParam & MK_LBUTTON) && !app->draggingTimeline_) {
            const TimelineHit hit = app->hitTimeline(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit.valid) app->selectTimelineTime(hit.globalTime);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app->draggingTimeline_) {
            const TimelineHit hit = app->hitTimeline(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit.valid && app->draggingClipId_ != 0) app->reorderSelected(hit.index);
            app->draggingTimeline_ = false;
            app->draggingClipId_ = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        const TimelineHit hit = app->hitTimeline(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hit.valid) app->selectTimelineTime(hit.globalTime, true);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK App::InspectorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = WindowApp(window);
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND: {
        RECT area{};
        GetClientRect(window, &area);
        FillRect(reinterpret_cast<HDC>(wParam), &area, app->panelBrush_);
        return 1;
    }
    case WM_COMMAND:
        return SendMessageW(app->mainWindow_, WM_COMMAND, wParam, lParam);
    case WM_HSCROLL:
        return SendMessageW(app->mainWindow_, WM_HSCROLL, wParam, lParam);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return SendMessageW(app->mainWindow_, message, wParam, lParam);
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK App::StillWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = WindowApp(window);
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC context = BeginPaint(window, &paint);
        app->paintStill(window, context);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK App::SplashWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = WindowApp(window);
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC context = BeginPaint(window, &paint);
        app->paintSplash(window, context);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK App::CommandPanelProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = WindowApp(window);
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND: {
        RECT area{};
        GetClientRect(window, &area);
        FillRect(reinterpret_cast<HDC>(wParam), &area, app->panelBrush_);
        return 1;
    }
    case WM_COMMAND:
        return SendMessageW(app->mainWindow_, WM_COMMAND, wParam, lParam);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return SendMessageW(app->mainWindow_, message, wParam, lParam);
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK App::CommandEditSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR referenceData) {
    auto* app = reinterpret_cast<App*>(referenceData);
    if (message == WM_KEYDOWN && app) {
        if (wParam == VK_RETURN) {
            app->executeCommandSelection();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            app->hideCommandBar();
            return 0;
        }
        if (wParam == VK_DOWN || wParam == VK_UP) {
            const LRESULT count = SendMessageW(app->commandList_, LB_GETCOUNT, 0, 0);
            if (count > 0) {
                int selected = static_cast<int>(SendMessageW(app->commandList_, LB_GETCURSEL, 0, 0));
                if (selected == LB_ERR) selected = 0;
                selected = wParam == VK_DOWN ? std::min(selected + 1, static_cast<int>(count) - 1) : std::max(0, selected - 1);
                SendMessageW(app->commandList_, LB_SETCURSEL, selected, 0);
            }
            return 0;
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT App::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {1000, 650};
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC context = BeginPaint(window, &paint);
        paintMain(context);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_COMMAND:
        handleCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_HSCROLL:
        handleInspectorScroll(reinterpret_cast<HWND>(lParam), LOWORD(wParam));
        return 0;
    case WM_DRAWITEM:
        if (wParam == ID_MEDIA_LIST) {
            drawMediaItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC context = reinterpret_cast<HDC>(wParam);
        SetTextColor(context, kText);
        SetBkColor(context, kPanel);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC context = reinterpret_cast<HDC>(wParam);
        SetTextColor(context, kText);
        SetBkColor(context, kInput);
        return reinterpret_cast<LRESULT>(inputBrush_);
    }
    case WM_CTLCOLORBTN: {
        HDC context = reinterpret_cast<HDC>(wParam);
        SetTextColor(context, kText);
        SetBkColor(context, kPanel);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }
    case WM_DROPFILES:
        handleDrop(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_KEYDOWN:
        if (handleShortcut(wParam)) return 0;
        break;
    case WM_TIMER:
        if (wParam == TIMER_SPLASH) {
            KillTimer(mainWindow_, TIMER_SPLASH);
            if (splashWindow_) DestroyWindow(splashWindow_);
            splashWindow_ = nullptr;
            ShowWindow(mainWindow_, SW_SHOW);
            SetForegroundWindow(mainWindow_);
        } else if (wParam == TIMER_PLAYHEAD) {
            updatePlayhead();
        } else if (wParam == TIMER_AUTOSAVE) {
            autosave();
        } else if (wParam == TIMER_GRADE_DEBOUNCE) {
            KillTimer(mainWindow_, TIMER_GRADE_DEBOUNCE);
            renderAccuratePreview();
        }
        return 0;
    case WM_FILLEMA_PROBE_COMPLETE: {
        std::unique_ptr<ProbeCompletion> completion(reinterpret_cast<ProbeCompletion*>(lParam));
        if (completion) {
            if (MediaItem* media = project_.findMedia(completion->mediaId)) {
                if (completion->info.valid) {
                    const double previousDuration = media->duration;
                    media->codec = completion->info.codec;
                    media->width = completion->info.width;
                    media->height = completion->info.height;
                    media->fps = completion->info.fps;
                    media->duration = completion->info.duration;
                    media->hasAudio = completion->info.hasAudio;
                    media->probeComplete = true;
                    if (project_.media.size() == 1 && project_.output.width == 1920 && project_.output.height == 1080) {
                        project_.output.width = media->width;
                        project_.output.height = media->height;
                        project_.output.fps = media->fps;
                    }
                    for (auto& clip : project_.timeline) {
                        if (clip.mediaId == media->id && (clip.outPoint <= 5.001 || std::abs(clip.outPoint - previousDuration) < 0.01)) {
                            clip.outPoint = media->duration;
                        }
                    }
                    refreshMediaList();
                    maybeCreateProxy(media->id, *media);
                    setStatus(Utf8ToWide(adaptiveProfile_.label) + L" · 미디어 분석 완료");
                } else {
                    setStatus(L"미디어 정보를 읽지 못했습니다. 재생은 계속 시도합니다.");
                }
            }
        }
        return 0;
    }
    case WM_FILLEMA_PROXY_COMPLETE: {
        std::unique_ptr<ProxyCompletion> completion(reinterpret_cast<ProxyCompletion*>(lParam));
        if (completion) {
            if (MediaItem* media = project_.findMedia(completion->mediaId)) {
                media->proxyPath = completion->path;
                media->proxyReady = completion->success;
                refreshMediaList();
                setStatus(completion->success ? L"성능용 편집본 준비 완료 · 최종 출력은 원본을 사용합니다."
                                              : L"성능용 편집본 생성에 실패해 원본으로 편집합니다.");
            }
        }
        return 0;
    }
    case WM_FILLEMA_PREVIEW_READY: {
        std::unique_ptr<PreviewCompletion> completion(reinterpret_cast<PreviewCompletion*>(lParam));
        if (completion && completion->clipId == selectedClipId_ && completion->generation == previewGeneration_ && completion->success) {
            clearPreviewBitmap();
            previewBitmap_ = reinterpret_cast<HBITMAP>(LoadImageW(nullptr, completion->path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE));
            if (previewBitmap_) {
                showAccurateStill_ = true;
                ShowWindow(stillWindow_, SW_SHOW);
                InvalidateRect(stillWindow_, nullptr, FALSE);
                setStatus(L"정확 색감 미리보기 · 최종 출력과 같은 색 처리");
            }
        }
        return 0;
    }
    case WM_FILLEMA_EXPORT_COMPLETE: {
        std::unique_ptr<ExportCompletion> completion(reinterpret_cast<ExportCompletion*>(lParam));
        exporting_ = false;
        EnableWindow(GetDlgItem(mainWindow_, ID_TOOL_EXPORT), TRUE);
        if (completion && completion->success) {
            setStatus(L"영상 출력 완료 · " + completion->output.wstring());
            MessageBoxW(mainWindow_, (L"영상 출력을 마쳤습니다.\n\n" + completion->output.wstring()).c_str(),
                L"Fillema", MB_OK | MB_ICONINFORMATION);
        } else {
            const std::wstring error = completion ? completion->error : L"알 수 없는 오류";
            setStatus(L"영상 출력 실패");
            MessageBoxW(mainWindow_, (L"영상을 출력하지 못했습니다.\n\n" + error).c_str(), L"Fillema", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_CLOSE:
        if (confirmDiscardChanges()) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void App::paintTimeline(HWND window, HDC context) {
    RECT area{};
    GetClientRect(window, &area);
    FillRect(context, &area, panelBrush_);
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, kMuted);
    SelectObject(context, uiFont_);

    const int rulerHeight = 34;
    HBRUSH rulerBrush = CreateSolidBrush(kPanelRaised);
    RECT ruler{0, 0, area.right, rulerHeight};
    FillRect(context, &ruler, rulerBrush);
    DeleteObject(rulerBrush);

    const double total = project_.timelineDuration();
    if (total <= 0.0) {
        SetTextColor(context, kText);
        SelectObject(context, semiboldFont_);
        RECT title{18, 66, area.right - 18, 96};
        DrawTextW(context, L"타임라인이 비어 있습니다", -1, &title, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(context, kMuted);
        SelectObject(context, uiFont_);
        RECT help{18, 100, area.right - 18, 150};
        DrawTextW(context, L"미디어를 두 번 누르거나 파일을 이 창에 끌어놓으세요.", -1, &help,
            DT_CENTER | DT_TOP | DT_WORDBREAK);
        return;
    }

    const int left = 12;
    const int right = std::max(left + 1, static_cast<int>(area.right) - 12);
    const int usable = right - left;
    const int videoTop = 48;
    const int videoBottom = std::max(videoTop + 52, (static_cast<int>(area.bottom) + videoTop) / 2);
    const int audioTop = videoBottom + 8;
    const int audioBottom = std::max(audioTop + 34, static_cast<int>(area.bottom) - 12);

    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(47, 48, 54));
    const HGDIOBJ previousPen = SelectObject(context, gridPen);
    const double major = total > 600.0 ? 60.0 : total > 120.0 ? 30.0 : total > 30.0 ? 5.0 : 1.0;
    for (double second = 0.0; second <= total + 0.001; second += major) {
        const int x = left + static_cast<int>(std::round(second / total * usable));
        MoveToEx(context, x, 23, nullptr);
        LineTo(context, x, area.bottom);
        RECT label{x + 4, 2, x + 90, 23};
        const std::wstring time = formatTime(second).substr(0, 5);
        DrawTextW(context, time.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(context, previousPen);
    DeleteObject(gridPen);

    double cursor = 0.0;
    for (const auto& clip : project_.timeline) {
        const double duration = clip.timelineDuration();
        const int x1 = left + static_cast<int>(std::round(cursor / total * usable));
        const int x2 = left + static_cast<int>(std::round((cursor + duration) / total * usable));
        const bool selected = clip.id == selectedClipId_;
        RECT video{x1 + 1, videoTop, std::max(x1 + 7, x2 - 1), videoBottom};
        RECT audio{x1 + 1, audioTop, std::max(x1 + 7, x2 - 1), audioBottom};
        HBRUSH videoBrush = CreateSolidBrush(selected ? kAccentSoft : kTimelineClip);
        HBRUSH audioBrush = CreateSolidBrush(selected ? RGB(73, 91, 77) : kTimelineAudio);
        FillRect(context, &video, videoBrush);
        FillRect(context, &audio, audioBrush);
        DeleteObject(videoBrush);
        DeleteObject(audioBrush);

        HPEN clipBorder = CreatePen(PS_SOLID, selected ? 2 : 1, selected ? kAccent : kBorder);
        const HGDIOBJ old = SelectObject(context, clipBorder);
        const HGDIOBJ oldBrush = SelectObject(context, GetStockObject(HOLLOW_BRUSH));
        Rectangle(context, video.left, video.top, video.right, video.bottom);
        Rectangle(context, audio.left, audio.top, audio.right, audio.bottom);
        SelectObject(context, oldBrush);
        SelectObject(context, old);
        DeleteObject(clipBorder);

        if (video.right - video.left > 42) {
            const MediaItem* media = project_.findMedia(clip.mediaId);
            std::wstring name = media ? Utf8ToWide(media->displayName) : L"원본 없음";
            if (clip.speed != 1.0) name += L"  " + std::to_wstring(clip.speed).substr(0, 3) + L"×";
            RECT text{video.left + 8, video.top + 7, video.right - 6, video.bottom - 5};
            SetTextColor(context, kText);
            DrawTextW(context, name.c_str(), -1, &text, DT_LEFT | DT_TOP | DT_END_ELLIPSIS | DT_SINGLELINE);
            RECT durationText{video.left + 8, video.top + 28, video.right - 6, video.bottom - 4};
            SetTextColor(context, RGB(195, 196, 203));
            const std::wstring value = formatTime(duration);
            DrawTextW(context, value.c_str(), -1, &durationText, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
        cursor += duration;
    }

    const int playheadX = left + static_cast<int>(std::round(std::clamp(playheadTimeline_, 0.0, total) / total * usable));
    HPEN playhead = CreatePen(PS_SOLID, 2, kAccent);
    const HGDIOBJ old = SelectObject(context, playhead);
    MoveToEx(context, playheadX, 25, nullptr);
    LineTo(context, playheadX, area.bottom);
    POINT marker[3]{{playheadX - 6, 25}, {playheadX + 6, 25}, {playheadX, 34}};
    HBRUSH markerBrush = CreateSolidBrush(kAccent);
    const HGDIOBJ oldBrush = SelectObject(context, markerBrush);
    Polygon(context, marker, 3);
    SelectObject(context, oldBrush);
    SelectObject(context, old);
    DeleteObject(markerBrush);
    DeleteObject(playhead);
}

void App::paintStill(HWND window, HDC context) {
    RECT area{};
    GetClientRect(window, &area);
    FillRect(context, &area, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (window == stillWindow_ && previewBitmap_) {
        BITMAP bitmap{};
        GetObjectW(previewBitmap_, sizeof(bitmap), &bitmap);
        const double scale = std::min(
            static_cast<double>(std::max(1L, area.right)) / std::max(1L, bitmap.bmWidth),
            static_cast<double>(std::max(1L, area.bottom)) / std::max(1L, bitmap.bmHeight));
        const int width = std::max(1, static_cast<int>(bitmap.bmWidth * scale));
        const int height = std::max(1, static_cast<int>(bitmap.bmHeight * scale));
        const int x = (area.right - width) / 2;
        const int y = (area.bottom - height) / 2;
        HDC memory = CreateCompatibleDC(context);
        const HGDIOBJ old = SelectObject(memory, previewBitmap_);
        SetStretchBltMode(context, HALFTONE);
        StretchBlt(context, x, y, width, height, memory, 0, 0, bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
        return;
    }
    if (window == previewWindow_ && selectedClipId_ == 0) {
        SetBkMode(context, TRANSPARENT);
        SelectObject(context, semiboldFont_);
        SetTextColor(context, RGB(220, 220, 224));
        RECT title{20, area.bottom / 2 - 35, area.right - 20, area.bottom / 2};
        DrawTextW(context, L"Fillema", -1, &title, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(context, uiFont_);
        SetTextColor(context, RGB(125, 126, 134));
        RECT help{20, area.bottom / 2, area.right - 20, area.bottom / 2 + 42};
        DrawTextW(context, L"빠르고 정교한 시네마틱 영상 편집 도구", -1, &help, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
}

void App::paintSplash(HWND window, HDC context) {
    RECT area{};
    GetClientRect(window, &area);
    for (int x = 0; x < area.right; ++x) {
        const double t = static_cast<double>(x) / std::max(1, static_cast<int>(area.right) - 1);
        const int red = static_cast<int>(24 + t * 20);
        const int green = static_cast<int>(22 + t * 4);
        const int blue = static_cast<int>(30 + t * 18);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(red, green, blue));
        const HGDIOBJ old = SelectObject(context, pen);
        MoveToEx(context, x, 0, nullptr);
        LineTo(context, x, area.bottom);
        SelectObject(context, old);
        DeleteObject(pen);
    }
    HBRUSH accent = CreateSolidBrush(kAccent);
    RECT mark{42, 46, 104, 108};
    FillRect(context, &mark, accent);
    DeleteObject(accent);
    SetBkMode(context, TRANSPARENT);
    HFONT markFont = CreateFontW(-50, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SelectObject(context, markFont);
    SetTextColor(context, RGB(255, 255, 255));
    DrawTextW(context, L"F", -1, &mark, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(markFont);
    SelectObject(context, semiboldFont_);
    RECT name{124, 46, area.right - 30, 84};
    DrawTextW(context, L"FEBIUS FILLEMA", -1, &name, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(context, uiFont_);
    SetTextColor(context, RGB(186, 185, 193));
    RECT tagline{124, 82, area.right - 30, 116};
    DrawTextW(context, L"빠르고 정교한 시네마틱 영상 편집 도구.", -1, &tagline, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(context, RGB(127, 126, 137));
    RECT series{42, area.bottom - 48, area.right - 30, area.bottom - 20};
    DrawTextW(context, L"CREATOR SERIES  ·  VERSION 0.1", -1, &series, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void App::drawMediaItem(const DRAWITEMSTRUCT& draw) {
    if (draw.itemID == static_cast<UINT>(-1)) return;
    HDC context = draw.hDC;
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    HBRUSH background = CreateSolidBrush(selected ? kAccentSoft : kPanel);
    FillRect(context, &draw.rcItem, background);
    DeleteObject(background);
    if (draw.itemID >= project_.media.size()) return;
    const MediaItem& media = project_.media[draw.itemID];
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, kText);
    SelectObject(context, semiboldFont_);
    RECT name{draw.rcItem.left + 12, draw.rcItem.top + 8, draw.rcItem.right - 8, draw.rcItem.top + 30};
    const std::wstring wideName = Utf8ToWide(media.displayName);
    DrawTextW(context, wideName.c_str(), -1, &name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(context, uiFont_);
    SetTextColor(context, selected ? RGB(225, 218, 216) : kMuted);
    RECT details{draw.rcItem.left + 12, draw.rcItem.top + 32, draw.rcItem.right - 8, draw.rcItem.bottom - 5};
    const std::wstring line = mediaDisplayLine(media);
    DrawTextW(context, line.c_str(), -1, &details, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (draw.itemState & ODS_FOCUS) DrawFocusRect(context, &draw.rcItem);
}

void App::handleCommand(int id, int notification, HWND source) {
    switch (id) {
    case ID_FILE_NEW: newProject(); return;
    case ID_FILE_OPEN: openProject(); return;
    case ID_FILE_SAVE: saveProject(false); return;
    case ID_FILE_SAVE_AS: saveProject(true); return;
    case ID_FILE_IMPORT:
    case ID_TOOL_IMPORT: importDialog(); return;
    case ID_FILE_EXPORT:
    case ID_TOOL_EXPORT:
    case ID_OUTPUT_EXPORT: exportDialog(); return;
    case ID_FILE_EXIT: SendMessageW(mainWindow_, WM_CLOSE, 0, 0); return;
    case ID_EDIT_UNDO:
    case ID_TOOL_UNDO: undo(); return;
    case ID_EDIT_REDO:
    case ID_TOOL_REDO: redo(); return;
    case ID_EDIT_SPLIT:
    case ID_TOOL_SPLIT:
    case ID_PROP_SPLIT: splitSelected(); return;
    case ID_EDIT_RIPPLE_DELETE:
    case ID_PROP_DELETE: rippleDeleteSelected(); return;
    case ID_EDIT_DUPLICATE:
    case ID_PROP_DUPLICATE: duplicateSelected(); return;
    case ID_EDIT_COPY: copySelected(); return;
    case ID_EDIT_PASTE: pasteClip(); return;
    case ID_PROP_TRIM_HEAD: trimSelected(true); return;
    case ID_PROP_TRIM_TAIL: trimSelected(false); return;
    case ID_VIEW_COMMAND_BAR: showCommandBar(); return;
    case ID_VIEW_FULLSCREEN: setFullscreen(!fullscreen_); return;
    case ID_HELP_ABOUT: showAbout(); return;
    case ID_PLAY_PAUSE: togglePlayback(); return;
    case ID_TAB_COLOR_BASIC: switchInspectorTab(0); return;
    case ID_TAB_COLOR_PRECISE: switchInspectorTab(1); return;
    case ID_TAB_CLIP: switchInspectorTab(2); return;
    case ID_TAB_AUDIO: switchInspectorTab(3); return;
    case ID_TAB_TEXT: switchInspectorTab(4); return;
    case ID_TAB_EXPORT: switchInspectorTab(5); return;
    default: break;
    }

    if (id == ID_MEDIA_LIST) {
        if (notification == LBN_SELCHANGE) {
            selectedMediaIndex_ = static_cast<int>(SendMessageW(mediaList_, LB_GETCURSEL, 0, 0));
        } else if (notification == LBN_DBLCLK) {
            const int index = static_cast<int>(SendMessageW(mediaList_, LB_GETCURSEL, 0, 0));
            if (index >= 0) addMediaToTimeline(static_cast<std::size_t>(index));
        }
        return;
    }
    if (id == ID_PERFORMANCE_MODE && notification == CBN_SELCHANGE) {
        const int index = static_cast<int>(SendMessageW(performanceCombo_, CB_GETCURSEL, 0, 0));
        recordUndo();
        project_.performanceMode = index == 1 ? PerformanceMode::Quality
            : index == 2 ? PerformanceMode::Balanced
            : index == 3 ? PerformanceMode::Speed : PerformanceMode::Automatic;
        adaptiveProfile_ = ChooseAdaptiveProfile(project_.performanceMode, logicalProcessors_, physicalMemoryMb_,
            selectedMediaIndex_ >= 0 && static_cast<std::size_t>(selectedMediaIndex_) < project_.media.size()
                ? &project_.media[static_cast<std::size_t>(selectedMediaIndex_)] : nullptr);
        markDirty();
        setStatus(Utf8ToWide(adaptiveProfile_.label) + L" · " + Utf8ToWide(adaptiveProfile_.reason));
        for (const auto& media : project_.media) {
            if (media.probeComplete && !media.proxyReady) maybeCreateProxy(media.id, media);
        }
        return;
    }
    if (id == ID_COMMAND_EDIT && notification == EN_CHANGE) {
        filterCommands();
        return;
    }
    if (id == ID_COMMAND_LIST && notification == LBN_DBLCLK) {
        executeCommandSelection();
        return;
    }

    Clip* clip = selectedClip();
    if (id == ID_PROP_LOOK && clip && notification == CBN_SELCHANGE) {
        recordUndo();
        const int index = static_cast<int>(SendMessageW(source, CB_GETCURSEL, 0, 0));
        clip = selectedClip();
        clip->color.look = static_cast<LookPreset>(std::clamp(index, 0, 7));
        if (clip->color.look != LookPreset::None && clip->color.lookStrength <= 0.0) clip->color.lookStrength = 65.0;
        markDirty();
        buildInspector();
        scheduleAccuratePreview();
        return;
    }
    if (id == ID_PROP_MUTE && clip && notification == BN_CLICKED) {
        recordUndo(); clip = selectedClip(); clip->audio.muted = Button_GetCheck(source) == BST_CHECKED; markDirty(); return;
    }
    if (id == ID_PROP_NORMALIZE && clip && notification == BN_CLICKED) {
        recordUndo(); clip = selectedClip(); clip->audio.normalize = Button_GetCheck(source) == BST_CHECKED; markDirty(); return;
    }
    if (id == ID_TEXT_ADD && clip && notification == BN_CLICKED) {
        recordUndo();
        clip = selectedClip();
        if (clip->texts.empty()) {
            TextOverlay text;
            text.id = project_.allocateId();
            text.text = "새 텍스트";
            clip->texts.push_back(std::move(text));
        }
        markDirty();
        buildInspector();
        scheduleAccuratePreview();
        return;
    }
    if (id == ID_TEXT_DELETE && clip && notification == BN_CLICKED) {
        if (!clip->texts.empty()) {
            recordUndo(); clip = selectedClip(); clip->texts.erase(clip->texts.begin()); markDirty(); buildInspector(); scheduleAccuratePreview();
        }
        return;
    }
    if (id == ID_TEXT_CONTENT && clip && notification == EN_CHANGE && !buildingInspector_ && !clip->texts.empty()) {
        if (activeSliderId_ != ID_TEXT_CONTENT) { recordUndo(); activeSliderId_ = ID_TEXT_CONTENT; }
        clip = selectedClip();
        clip->texts.front().text = WideToUtf8(WindowText(source));
        markDirty(); scheduleAccuratePreview();
        return;
    }
    if (id == ID_TEXT_FONT && clip && notification == CBN_SELCHANGE && !clip->texts.empty()) {
        recordUndo(); clip = selectedClip();
        const int index = static_cast<int>(SendMessageW(source, CB_GETCURSEL, 0, 0));
        static constexpr std::array<const char*, 4> fonts{"Malgun Gothic", "Segoe UI", "Arial", "Georgia"};
        clip->texts.front().font = fonts[static_cast<std::size_t>(std::clamp(index, 0, 3))];
        markDirty(); scheduleAccuratePreview(); return;
    }
    if (id == ID_TEXT_POSITION && clip && notification == CBN_SELCHANGE && !clip->texts.empty()) {
        recordUndo(); clip = selectedClip();
        clip->texts.front().position = static_cast<TextPosition>(std::clamp(
            static_cast<int>(SendMessageW(source, CB_GETCURSEL, 0, 0)), 0, 3));
        markDirty(); scheduleAccuratePreview(); return;
    }
    if (id == ID_OUTPUT_RESOLUTION && notification == CBN_SELCHANGE) {
        recordUndo();
        const int index = static_cast<int>(SendMessageW(source, CB_GETCURSEL, 0, 0));
        if (index == 0) { project_.output.width = 1280; project_.output.height = 720; }
        else if (index == 2) { project_.output.width = 3840; project_.output.height = 2160; }
        else { project_.output.width = 1920; project_.output.height = 1080; }
        markDirty(); return;
    }
    if (id == ID_OUTPUT_CODEC && notification == CBN_SELCHANGE) {
        recordUndo();
        project_.output.codec = SendMessageW(source, CB_GETCURSEL, 0, 0) == 1 ? VideoCodec::H265 : VideoCodec::H264;
        markDirty(); return;
    }
    if (id == ID_OUTPUT_QUALITY && notification == CBN_SELCHANGE) {
        recordUndo();
        const int index = static_cast<int>(SendMessageW(source, CB_GETCURSEL, 0, 0));
        project_.output.quality = index == 0 ? ExportQuality::Compact : index == 1 ? ExportQuality::Standard : ExportQuality::High;
        markDirty(); return;
    }
    if (id == ID_OUTPUT_AUDIO && notification == BN_CLICKED) {
        recordUndo(); project_.output.preserveAudio = Button_GetCheck(source) == BST_CHECKED; markDirty(); return;
    }
}

void App::handleInspectorScroll(HWND control, int request) {
    if (!control) return;
    const int id = GetDlgCtrlID(control);
    const int position = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
    if (activeSliderId_ != id) {
        recordUndo();
        activeSliderId_ = id;
    }
    Clip* clip = selectedClip();
    bool colorChanged = false;
    if (clip) {
        switch (id) {
        case ID_PROP_EXPOSURE: clip->color.exposure = position / 100.0; updateValueLabel(id, std::to_wstring(position / 100.0).substr(0, 4)); colorChanged = true; break;
        case ID_PROP_CONTRAST: clip->color.contrast = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_HIGHLIGHTS: clip->color.highlights = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_SHADOWS: clip->color.shadows = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_WHITES: clip->color.whites = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_BLACKS: clip->color.blacks = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_TEMPERATURE: clip->color.temperature = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_TINT: clip->color.tint = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_SATURATION: clip->color.saturation = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_LOOK_STRENGTH: clip->color.lookStrength = position; updateValueLabel(id, std::to_wstring(position) + L"%"); colorChanged = true; break;
        case ID_PROP_GRAIN: clip->color.grain = position; updateValueLabel(id, std::to_wstring(position) + L"%"); colorChanged = true; break;
        case ID_PROP_VIGNETTE: clip->color.vignette = position; updateValueLabel(id, std::to_wstring(position) + L"%"); colorChanged = true; break;
        case ID_PROP_LETTERBOX: clip->color.letterbox = position / 10.0; updateValueLabel(id, std::to_wstring(position / 10.0).substr(0, 4) + L"%"); colorChanged = true; break;
        case ID_PROP_CURVE_SHADOWS: clip->color.curveShadows = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_CURVE_MIDTONES: clip->color.curveMidtones = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_CURVE_HIGHLIGHTS: clip->color.curveHighlights = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; break;
        case ID_PROP_SHADOW_R: clip->color.shadowWheel[0] = position; colorChanged = true; break;
        case ID_PROP_SHADOW_G: clip->color.shadowWheel[1] = position; colorChanged = true; break;
        case ID_PROP_SHADOW_B: clip->color.shadowWheel[2] = position; colorChanged = true; break;
        case ID_PROP_MID_R: clip->color.midtoneWheel[0] = position; colorChanged = true; break;
        case ID_PROP_MID_G: clip->color.midtoneWheel[1] = position; colorChanged = true; break;
        case ID_PROP_MID_B: clip->color.midtoneWheel[2] = position; colorChanged = true; break;
        case ID_PROP_HIGH_R: clip->color.highlightWheel[0] = position; colorChanged = true; break;
        case ID_PROP_HIGH_G: clip->color.highlightWheel[1] = position; colorChanged = true; break;
        case ID_PROP_HIGH_B: clip->color.highlightWheel[2] = position; colorChanged = true; break;
        case ID_PROP_SPEED: clip->speed = position / 100.0; updateValueLabel(id, std::to_wstring(position / 100.0).substr(0, 4) + L"×"); InvalidateRect(timelineWindow_, nullptr, FALSE); break;
        case ID_PROP_VOLUME: clip->audio.volumeDb = position / 10.0; updateValueLabel(id, std::to_wstring(position / 10.0).substr(0, 5) + L" dB"); break;
        case ID_PROP_FADE_IN: clip->audio.fadeIn = position / 100.0; updateValueLabel(id, std::to_wstring(position / 100.0).substr(0, 4) + L"초"); break;
        case ID_PROP_FADE_OUT: clip->audio.fadeOut = position / 100.0; updateValueLabel(id, std::to_wstring(position / 100.0).substr(0, 4) + L"초"); break;
        case ID_PROP_EQ_LOW: clip->audio.eqLow = position / 10.0; updateValueLabel(id, std::to_wstring(position / 10.0).substr(0, 5) + L" dB"); break;
        case ID_PROP_EQ_MID: clip->audio.eqMid = position / 10.0; updateValueLabel(id, std::to_wstring(position / 10.0).substr(0, 5) + L" dB"); break;
        case ID_PROP_EQ_HIGH: clip->audio.eqHigh = position / 10.0; updateValueLabel(id, std::to_wstring(position / 10.0).substr(0, 5) + L" dB"); break;
        case ID_TEXT_SIZE:
            if (!clip->texts.empty()) { clip->texts.front().fontSize = position; updateValueLabel(id, std::to_wstring(position)); colorChanged = true; }
            break;
        case ID_TEXT_OPACITY:
            if (!clip->texts.empty()) { clip->texts.front().opacity = position / 100.0; updateValueLabel(id, std::to_wstring(position) + L"%"); colorChanged = true; }
            break;
        case ID_TEXT_FADE_IN:
            if (!clip->texts.empty()) { clip->texts.front().fadeIn = position / 100.0; updateValueLabel(id, std::to_wstring(position / 100.0).substr(0, 4) + L"초"); colorChanged = true; }
            break;
        case ID_TEXT_FADE_OUT:
            if (!clip->texts.empty()) { clip->texts.front().fadeOut = position / 100.0; updateValueLabel(id, std::to_wstring(position / 100.0).substr(0, 4) + L"초"); colorChanged = true; }
            break;
        default: break;
        }
    }
    markDirty();
    if (colorChanged) scheduleAccuratePreview();
    if (request == TB_ENDTRACK || request == TB_THUMBPOSITION) activeSliderId_ = -1;
}

void App::handleDrop(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::filesystem::path> paths;
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);
        paths.emplace_back(std::move(path));
    }
    DragFinish(drop);
    importPaths(paths, true);
}

bool App::handleShortcut(WPARAM key) {
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const HWND focus = GetFocus();
    if (key == VK_ESCAPE) {
        if (IsWindowVisible(commandPanel_)) hideCommandBar();
        else if (fullscreen_) setFullscreen(false);
        return true;
    }
    if (IsEditLike(focus) && !control) return false;
    if (control) {
        switch (key) {
        case 'N': newProject(); return true;
        case 'O': openProject(); return true;
        case 'S': saveProject(shift); return true;
        case 'I': importDialog(); return true;
        case 'E': exportDialog(); return true;
        case 'Z': undo(); return true;
        case 'Y': redo(); return true;
        case 'D': duplicateSelected(); return true;
        case 'C': copySelected(); return true;
        case 'V': pasteClip(); return true;
        case 'K': splitSelected(); return true;
        case 'P': showCommandBar(); return true;
        default: break;
        }
    } else {
        switch (key) {
        case VK_SPACE: togglePlayback(); return true;
        case VK_DELETE: rippleDeleteSelected(); return true;
        case 'S': splitSelected(); return true;
        case 'Q': trimSelected(true); return true;
        case 'W': trimSelected(false); return true;
        case 'F': setFullscreen(!fullscreen_); return true;
        case 'C': switchInspectorTab(0); return true;
        case 'T': handleCommand(ID_TEXT_ADD, BN_CLICKED, nullptr); switchInspectorTab(4); return true;
        default: break;
        }
    }
    return false;
}

void App::showAbout() {
    const std::wstring text =
        L"Febius Fillema 0.1\n\n"
        L"빠르고 정교한 시네마틱 영상 편집 도구.\n\n"
        L"네이티브 Windows UI · FFmpeg 미디어 엔진\n"
        L"프로젝트 형식: .fillema\n\n"
        L"Copyright © 2026 NokMyo. All rights reserved.";
    MessageBoxW(mainWindow_, text.c_str(), L"Fillema 정보", MB_OK | MB_ICONINFORMATION);
}

bool App::confirmDiscardChanges() {
    if (!project_.dirty) return true;
    const int result = MessageBoxW(mainWindow_, L"저장하지 않은 편집 내용이 있습니다. 저장하시겠습니까?",
        L"Fillema", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (result == IDCANCEL) return false;
    if (result == IDYES) return saveProject(false);
    return true;
}

void App::newProject() {
    if (!confirmDiscardChanges()) return;
    player_.pause();
    project_ = Project{};
    undoStack_.clear();
    redoStack_.clear();
    copiedClip_.reset();
    selectedClipId_ = 0;
    selectedMediaIndex_ = -1;
    playheadTimeline_ = 0.0;
    clearPreviewBitmap();
    refreshFromProject();
    setStatus(L"새 프로젝트 · 영상을 넣으면 설정을 자동으로 맞춥니다.");
}

void App::openProject() {
    if (!confirmDiscardChanges()) return;
    std::array<wchar_t, 32768> file{};
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = mainWindow_;
    dialog.lpstrFilter = L"Fillema 프로젝트 (*.fillema)\0*.fillema\0모든 파일 (*.*)\0*.*\0\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return;
    try {
        player_.pause();
        project_ = LoadProjectFile(file.data());
        undoStack_.clear();
        redoStack_.clear();
        selectedClipId_ = project_.timeline.empty() ? 0 : project_.timeline.front().id;
        selectedMediaIndex_ = -1;
        playheadTimeline_ = 0.0;
        refreshFromProject();
        if (selectedClipId_) selectClip(selectedClipId_);
        setStatus(L"프로젝트를 열었습니다.");
    } catch (const std::exception& exception) {
        MessageBoxW(mainWindow_, (L"프로젝트를 열지 못했습니다.\n\n" + ErrorFromException(exception)).c_str(),
            L"Fillema", MB_OK | MB_ICONERROR);
    }
}

bool App::saveProject(bool saveAs) {
    std::filesystem::path path = project_.filePath;
    if (saveAs || path.empty()) {
        std::array<wchar_t, 32768> file{};
        if (!path.empty()) wcsncpy_s(file.data(), file.size(), path.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
        dialog.hwndOwner = mainWindow_;
        dialog.lpstrFilter = L"Fillema 프로젝트 (*.fillema)\0*.fillema\0\0";
        dialog.lpstrFile = file.data();
        dialog.nMaxFile = static_cast<DWORD>(file.size());
        dialog.lpstrDefExt = L"fillema";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!GetSaveFileNameW(&dialog)) return false;
        path = AddExtensionIfMissing(file.data(), L".fillema");
    }
    try {
        Project saved = project_;
        saved.filePath = path;
        saved.dirty = false;
        SaveProjectFile(saved, path);
        project_.filePath = path;
        project_.name = WideToUtf8(path.stem().wstring());
        project_.dirty = false;
        std::error_code error;
        std::filesystem::remove(recoveryPath(), error);
        updateWindowTitle();
        setStatus(L"프로젝트 저장 완료 · " + path.wstring());
        return true;
    } catch (const std::exception& exception) {
        MessageBoxW(mainWindow_, (L"프로젝트를 저장하지 못했습니다.\n\n" + ErrorFromException(exception)).c_str(),
            L"Fillema", MB_OK | MB_ICONERROR);
        return false;
    }
}

void App::autosave() {
    if (!project_.dirty || project_.timeline.empty()) return;
    try {
        SaveProjectFile(project_, recoveryPath());
        setStatus(L"자동 저장 완료 · 편집 흐름을 방해하지 않고 복구본을 갱신했습니다.");
    } catch (...) {
        setStatus(L"자동 저장에 실패했습니다. 저장 위치를 확인해 주세요.");
    }
}

std::filesystem::path App::recoveryPath() const {
    if (!project_.filePath.empty()) {
        return project_.filePath.parent_path() / (project_.filePath.stem().wstring() + L".autosave.fillema");
    }
    return cacheDirectory_.parent_path() / L"Untitled.autosave.fillema";
}

void App::refreshFromProject() {
    const int performance = project_.performanceMode == PerformanceMode::Quality ? 1
        : project_.performanceMode == PerformanceMode::Balanced ? 2
        : project_.performanceMode == PerformanceMode::Speed ? 3 : 0;
    SendMessageW(performanceCombo_, CB_SETCURSEL, performance, 0);
    adaptiveProfile_ = ChooseAdaptiveProfile(project_.performanceMode, logicalProcessors_, physicalMemoryMb_);
    refreshMediaList();
    buildInspector();
    updateWindowTitle();
    InvalidateRect(timelineWindow_, nullptr, FALSE);
    InvalidateRect(previewWindow_, nullptr, FALSE);
}

void App::refreshMediaList() {
    if (!mediaList_) return;
    const int previous = static_cast<int>(SendMessageW(mediaList_, LB_GETCURSEL, 0, 0));
    SendMessageW(mediaList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(mediaList_, LB_RESETCONTENT, 0, 0);
    for (const auto& media : project_.media) {
        SendMessageW(mediaList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Utf8ToWide(media.displayName).c_str()));
    }
    if (!project_.media.empty()) {
        const int selected = std::clamp(previous >= 0 ? previous : selectedMediaIndex_, 0, static_cast<int>(project_.media.size()) - 1);
        SendMessageW(mediaList_, LB_SETCURSEL, selected, 0);
        selectedMediaIndex_ = selected;
    } else {
        selectedMediaIndex_ = -1;
    }
    SendMessageW(mediaList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(mediaList_, nullptr, TRUE);
}

void App::updateWindowTitle() {
    std::wstring title = L"Febius Fillema — ";
    title += Utf8ToWide(project_.name);
    if (project_.dirty) title += L" *";
    SetWindowTextW(mainWindow_, title.c_str());
}

void App::setStatus(const std::wstring& text) {
    SetControlText(statusWindow_, L"  " + text);
}

void App::markDirty() {
    project_.dirty = true;
    updateWindowTitle();
}

void App::recordUndo() {
    undoStack_.push_back(project_);
    if (undoStack_.size() > 50) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
}

void App::undo() {
    if (undoStack_.empty()) return;
    redoStack_.push_back(project_);
    project_ = std::move(undoStack_.back());
    undoStack_.pop_back();
    project_.dirty = true;
    if (selectedClipId_ && !project_.findClip(selectedClipId_)) selectedClipId_ = project_.timeline.empty() ? 0 : project_.timeline.front().id;
    refreshFromProject();
    if (selectedClipId_) selectClip(selectedClipId_);
    setStatus(L"실행 취소");
}

void App::redo() {
    if (redoStack_.empty()) return;
    undoStack_.push_back(project_);
    project_ = std::move(redoStack_.back());
    redoStack_.pop_back();
    project_.dirty = true;
    if (selectedClipId_ && !project_.findClip(selectedClipId_)) selectedClipId_ = project_.timeline.empty() ? 0 : project_.timeline.front().id;
    refreshFromProject();
    if (selectedClipId_) selectClip(selectedClipId_);
    setStatus(L"다시 실행");
}

void App::importDialog() {
    std::vector<wchar_t> file(65536, L'\0');
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = mainWindow_;
    dialog.lpstrFilter = L"영상 파일\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.mts;*.m2ts\0모든 파일 (*.*)\0*.*\0\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.Flags = OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return;

    std::vector<std::filesystem::path> paths;
    const std::filesystem::path first(file.data());
    const wchar_t* cursor = file.data() + wcslen(file.data()) + 1;
    if (*cursor == L'\0') {
        paths.push_back(first);
    } else {
        while (*cursor) {
            paths.push_back(first / cursor);
            cursor += wcslen(cursor) + 1;
        }
    }
    importPaths(paths, false);
}

void App::importPaths(const std::vector<std::filesystem::path>& paths, bool addToTimeline) {
    std::vector<std::size_t> importedIndices;
    for (const auto& path : paths) {
        if (!std::filesystem::is_regular_file(path)) continue;
        if (_wcsicmp(path.extension().c_str(), L".fillema") == 0 && paths.size() == 1) {
            if (confirmDiscardChanges()) {
                try { project_ = LoadProjectFile(path); refreshFromProject(); }
                catch (const std::exception& exception) { MessageBoxW(mainWindow_, ErrorFromException(exception).c_str(), L"Fillema", MB_OK | MB_ICONERROR); }
            }
            return;
        }
        const auto existing = std::find_if(project_.media.begin(), project_.media.end(), [&](const MediaItem& item) {
            return _wcsicmp(item.path.c_str(), path.c_str()) == 0;
        });
        if (existing != project_.media.end()) {
            importedIndices.push_back(static_cast<std::size_t>(std::distance(project_.media.begin(), existing)));
            continue;
        }
        recordUndo();
        MediaItem media;
        media.id = project_.allocateId();
        media.path = path;
        media.displayName = WideToUtf8(path.filename().wstring());
        media.duration = 5.0;
        project_.media.push_back(media);
        const std::size_t index = project_.media.size() - 1;
        importedIndices.push_back(index);
        probeMedia(media.id, media.path);
        markDirty();
    }
    refreshMediaList();
    for (const std::size_t index : importedIndices) if (addToTimeline && index < project_.media.size()) addMediaToTimeline(index);
    if (!importedIndices.empty()) setStatus(L"미디어를 가져왔습니다. 분석은 화면을 멈추지 않고 진행됩니다.");
}

void App::probeMedia(std::int64_t mediaId, std::filesystem::path path) {
    const std::filesystem::path executable = ffprobePath_;
    const HWND target = mainWindow_;
    mediaWorker_.submit([executable, target, mediaId, path = std::move(path)] {
        auto completion = std::make_unique<ProbeCompletion>();
        completion->mediaId = mediaId;
        const ProcessResult result = RunHiddenProcess(executable, BuildProbeArguments(path));
        if (result.started && result.exitCode == 0) completion->info = ParseProbeOutput(result.output);
        else completion->error = result.error.empty() ? Utf8ToWide(result.output) : result.error;
        PostMessageW(target, WM_FILLEMA_PROBE_COMPLETE, 0, reinterpret_cast<LPARAM>(completion.release()));
    });
}

void App::maybeCreateProxy(std::int64_t mediaId, const MediaItem& media) {
    adaptiveProfile_ = ChooseAdaptiveProfile(project_.performanceMode, logicalProcessors_, physicalMemoryMb_, &media);
    if (!adaptiveProfile_.createProxy) return;
    const std::size_t hash = std::hash<std::wstring>{}(media.path.wstring());
    const std::filesystem::path proxy = cacheDirectory_ / (L"proxy-" + std::to_wstring(hash) + L".mp4");
    if (std::filesystem::exists(proxy)) {
        auto completion = std::make_unique<ProxyCompletion>();
        completion->mediaId = mediaId;
        completion->path = proxy;
        completion->success = true;
        PostMessageW(mainWindow_, WM_FILLEMA_PROXY_COMPLETE, 0, reinterpret_cast<LPARAM>(completion.release()));
        return;
    }
    const std::filesystem::path executable = ffmpegPath_;
    const AdaptiveProfile profile = adaptiveProfile_;
    const std::filesystem::path input = media.path;
    const HWND target = mainWindow_;
    mediaWorker_.submit([executable, target, mediaId, input, proxy, profile] {
        auto completion = std::make_unique<ProxyCompletion>();
        completion->mediaId = mediaId;
        completion->path = proxy;
        const ProcessResult result = RunHiddenProcess(executable, BuildProxyArguments(input, proxy, profile));
        completion->success = result.started && result.exitCode == 0 && std::filesystem::exists(proxy);
        PostMessageW(target, WM_FILLEMA_PROXY_COMPLETE, 0, reinterpret_cast<LPARAM>(completion.release()));
    });
    setStatus(L"성능용 편집본을 백그라운드에서 만들고 있습니다. UI는 계속 사용할 수 있습니다.");
}

void App::addMediaToTimeline(std::size_t mediaIndex) {
    if (mediaIndex >= project_.media.size()) return;
    recordUndo();
    const MediaItem& media = project_.media[mediaIndex];
    Clip clip;
    clip.id = project_.allocateId();
    clip.mediaId = media.id;
    clip.inPoint = 0.0;
    clip.outPoint = std::max(0.1, media.duration);
    project_.timeline.push_back(clip);
    selectedClipId_ = clip.id;
    playheadTimeline_ = clipTimelineStart(project_.timeline.size() - 1);
    markDirty();
    buildInspector();
    InvalidateRect(timelineWindow_, nullptr, FALSE);
    selectClip(clip.id, clip.inPoint);
}

double App::clipTimelineStart(std::size_t index) const {
    double start = 0.0;
    for (std::size_t i = 0; i < std::min(index, project_.timeline.size()); ++i) start += project_.timeline[i].timelineDuration();
    return start;
}

App::TimelineHit App::hitTimeline(HWND timelineWindow, int x, int y) const {
    TimelineHit hit;
    RECT area{};
    GetClientRect(timelineWindow, &area);
    if (project_.timeline.empty() || x < 12 || x > area.right - 12 || y < 28 || y > area.bottom) return hit;
    const double total = project_.timelineDuration();
    if (total <= 0.0) return hit;
    hit.globalTime = std::clamp(static_cast<double>(x - 12) / std::max(1, static_cast<int>(area.right) - 24) * total, 0.0, total);
    double cursor = 0.0;
    for (std::size_t index = 0; index < project_.timeline.size(); ++index) {
        const Clip& clip = project_.timeline[index];
        const double end = cursor + clip.timelineDuration();
        if (hit.globalTime <= end || index + 1 == project_.timeline.size()) {
            hit.index = index;
            hit.sourceTime = clip.inPoint + std::clamp(hit.globalTime - cursor, 0.0, clip.timelineDuration()) * clip.speed;
            hit.valid = true;
            return hit;
        }
        cursor = end;
    }
    return hit;
}

void App::selectClip(std::int64_t clipId, double sourceTime, bool autoplay) {
    Clip* clip = project_.findClip(clipId);
    if (!clip) return;
    const MediaItem* media = project_.findMedia(clip->mediaId);
    if (!media) return;
    selectedClipId_ = clipId;
    if (sourceTime < 0.0) sourceTime = clip->inPoint;
    sourceTime = std::clamp(sourceTime, clip->inPoint, std::max(clip->inPoint, clip->outPoint - 0.001));
    if (const auto index = project_.clipIndex(clipId)) {
        playheadTimeline_ = clipTimelineStart(*index) + (sourceTime - clip->inPoint) / clip->speed;
    }
    const std::filesystem::path playbackPath = media->proxyReady && std::filesystem::exists(media->proxyPath)
        ? media->proxyPath : media->path;
    player_.load(playbackPath, sourceTime, clip->outPoint, autoplay);
    if (!autoplay) scheduleAccuratePreview();
    else {
        showAccurateStill_ = false;
        ShowWindow(stillWindow_, SW_HIDE);
    }
    const auto mediaIterator = std::find_if(project_.media.begin(), project_.media.end(),
        [&](const MediaItem& item) { return item.id == media->id; });
    if (mediaIterator != project_.media.end()) {
        selectedMediaIndex_ = static_cast<int>(std::distance(project_.media.begin(), mediaIterator));
        SendMessageW(mediaList_, LB_SETCURSEL, selectedMediaIndex_, 0);
    }
    buildInspector();
    InvalidateRect(timelineWindow_, nullptr, FALSE);
    SetControlText(playButton_, autoplay ? L"Ⅱ  일시정지" : L"▶  재생");
}

void App::selectTimelineTime(double globalTime, bool autoplay) {
    const double total = project_.timelineDuration();
    if (total <= 0.0) return;
    globalTime = std::clamp(globalTime, 0.0, total);
    double cursor = 0.0;
    for (std::size_t index = 0; index < project_.timeline.size(); ++index) {
        const Clip& clip = project_.timeline[index];
        const double end = cursor + clip.timelineDuration();
        if (globalTime <= end || index + 1 == project_.timeline.size()) {
            const double sourceTime = clip.inPoint + std::clamp(globalTime - cursor, 0.0, clip.timelineDuration()) * clip.speed;
            selectClip(clip.id, sourceTime, autoplay);
            return;
        }
        cursor = end;
    }
}

void App::splitSelected() {
    const auto index = project_.clipIndex(selectedClipId_);
    Clip* clip = selectedClip();
    if (!index || !clip) return;
    double position = player_.ready() ? player_.position() : clip->inPoint + (playheadTimeline_ - clipTimelineStart(*index)) * clip->speed;
    position = std::clamp(position, clip->inPoint, clip->outPoint);
    if (position - clip->inPoint < 0.05 || clip->outPoint - position < 0.05) {
        setStatus(L"클립 양 끝에서 0.05초 이상 떨어진 위치에서 분할해 주세요.");
        return;
    }
    recordUndo();
    clip = selectedClip();
    Clip right = *clip;
    right.id = project_.allocateId();
    for (auto& text : right.texts) text.id = project_.allocateId();
    right.inPoint = position;
    clip->outPoint = position;
    project_.timeline.insert(project_.timeline.begin() + static_cast<std::ptrdiff_t>(*index + 1), right);
    selectedClipId_ = right.id;
    markDirty();
    InvalidateRect(timelineWindow_, nullptr, FALSE);
    selectClip(right.id, right.inPoint);
    setStatus(L"클립을 분할했습니다.");
}

void App::rippleDeleteSelected() {
    const auto index = project_.clipIndex(selectedClipId_);
    if (!index) return;
    recordUndo();
    project_.timeline.erase(project_.timeline.begin() + static_cast<std::ptrdiff_t>(*index));
    if (project_.timeline.empty()) {
        selectedClipId_ = 0;
        playheadTimeline_ = 0.0;
        player_.pause();
        clearPreviewBitmap();
        ShowWindow(stillWindow_, SW_HIDE);
    } else {
        const std::size_t next = std::min(*index, project_.timeline.size() - 1);
        selectedClipId_ = project_.timeline[next].id;
        selectClip(selectedClipId_, project_.timeline[next].inPoint);
    }
    markDirty();
    buildInspector();
    InvalidateRect(timelineWindow_, nullptr, FALSE);
    setStatus(L"클립을 리플 삭제해 뒤 클립을 자동으로 당겼습니다.");
}

void App::trimSelected(bool head) {
    Clip* clip = selectedClip();
    if (!clip) return;
    double position = player_.ready() ? player_.position() : clip->inPoint;
    position = std::clamp(position, clip->inPoint, clip->outPoint);
    if ((head && clip->outPoint - position < 0.05) || (!head && position - clip->inPoint < 0.05)) return;
    recordUndo();
    clip = selectedClip();
    if (head) clip->inPoint = position; else clip->outPoint = position;
    markDirty();
    InvalidateRect(timelineWindow_, nullptr, FALSE);
    selectClip(clip->id, head ? clip->inPoint : std::max(clip->inPoint, clip->outPoint - 0.001));
    setStatus(head ? L"앞부분을 현재 위치까지 리플 트림했습니다." : L"뒷부분을 현재 위치까지 리플 트림했습니다.");
}

void App::duplicateSelected() {
    const auto index = project_.clipIndex(selectedClipId_);
    const Clip* clip = selectedClip();
    if (!index || !clip) return;
    recordUndo();
    clip = selectedClip();
    Clip copy = *clip;
    copy.id = project_.allocateId();
    for (auto& text : copy.texts) text.id = project_.allocateId();
    project_.timeline.insert(project_.timeline.begin() + static_cast<std::ptrdiff_t>(*index + 1), copy);
    selectedClipId_ = copy.id;
    markDirty();
    InvalidateRect(timelineWindow_, nullptr, FALSE);
    selectClip(copy.id, copy.inPoint);
}

void App::copySelected() {
    if (const Clip* clip = selectedClip()) {
        copiedClip_ = *clip;
        setStatus(L"클립을 내부 편집 클립보드에 복사했습니다.");
    }
}

void App::pasteClip() {
    if (!copiedClip_) return;
    const auto selectedIndex = project_.clipIndex(selectedClipId_);
    recordUndo();
    Clip copy = *copiedClip_;
    copy.id = project_.allocateId();
    for (auto& text : copy.texts) text.id = project_.allocateId();
    const std::size_t target = selectedIndex ? *selectedIndex + 1 : project_.timeline.size();
    project_.timeline.insert(project_.timeline.begin() + static_cast<std::ptrdiff_t>(target), copy);
    selectedClipId_ = copy.id;
    markDirty();
    selectClip(copy.id, copy.inPoint);
}

void App::reorderSelected(std::size_t targetIndex) {
    const auto sourceIndex = project_.clipIndex(draggingClipId_ ? draggingClipId_ : selectedClipId_);
    if (!sourceIndex || targetIndex >= project_.timeline.size() || *sourceIndex == targetIndex) return;
    recordUndo();
    Clip moved = std::move(project_.timeline[*sourceIndex]);
    project_.timeline.erase(project_.timeline.begin() + static_cast<std::ptrdiff_t>(*sourceIndex));
    if (*sourceIndex < targetIndex) --targetIndex;
    project_.timeline.insert(project_.timeline.begin() + static_cast<std::ptrdiff_t>(targetIndex), std::move(moved));
    markDirty();
    InvalidateRect(timelineWindow_, nullptr, FALSE);
    setStatus(L"클립 순서를 바꿨습니다.");
}

void App::togglePlayback() {
    if (!selectedClip()) {
        if (!project_.timeline.empty()) selectClip(project_.timeline.front().id, project_.timeline.front().inPoint, true);
        return;
    }
    if (player_.playing()) {
        player_.pause();
        SetControlText(playButton_, L"▶  재생");
        scheduleAccuratePreview();
    } else {
        showAccurateStill_ = false;
        ShowWindow(stillWindow_, SW_HIDE);
        player_.play();
        SetControlText(playButton_, L"Ⅱ  일시정지");
        setStatus(L"경량 재생 미리보기 · 멈추면 정확 색감 프레임으로 전환합니다.");
    }
}

void App::updatePlayhead() {
    Clip* clip = selectedClip();
    const auto index = project_.clipIndex(selectedClipId_);
    if (!clip || !index) {
        SetControlText(timeLabel_, L"00:00.000 / 00:00.000");
        return;
    }
    double sourcePosition = player_.ready() ? player_.position() : clip->inPoint;
    sourcePosition = std::clamp(sourcePosition, clip->inPoint, clip->outPoint);
    playheadTimeline_ = clipTimelineStart(*index) + (sourcePosition - clip->inPoint) / clip->speed;
    const double total = project_.timelineDuration();
    SetControlText(timeLabel_, formatTime(playheadTimeline_) + L" / " + formatTime(total));
    if (player_.playing() && sourcePosition >= clip->outPoint - 0.025) {
        if (*index + 1 < project_.timeline.size()) {
            const Clip& next = project_.timeline[*index + 1];
            selectClip(next.id, next.inPoint, true);
        } else {
            player_.pause();
            SetControlText(playButton_, L"▶  재생");
            scheduleAccuratePreview();
        }
    }
    InvalidateRect(timelineWindow_, nullptr, FALSE);
}

Clip* App::selectedClip() { return project_.findClip(selectedClipId_); }
const Clip* App::selectedClip() const { return project_.findClip(selectedClipId_); }

std::wstring App::formatTime(double seconds) const {
    seconds = std::max(0.0, seconds);
    const int minutes = static_cast<int>(seconds) / 60;
    const int wholeSeconds = static_cast<int>(seconds) % 60;
    const int milliseconds = static_cast<int>(std::round((seconds - std::floor(seconds)) * 1000.0)) % 1000;
    std::wostringstream stream;
    stream << std::setfill(L'0') << std::setw(2) << minutes << L':' << std::setw(2) << wholeSeconds << L'.' << std::setw(3) << milliseconds;
    return stream.str();
}

std::wstring App::mediaDisplayLine(const MediaItem& media) const {
    if (!media.probeComplete) return L"분석 중…";
    std::wstring line = std::to_wstring(media.width) + L"×" + std::to_wstring(media.height);
    if (media.fps > 0.0) line += L" · " + std::to_wstring(media.fps).substr(0, 5) + L" fps";
    line += L" · " + formatTime(media.duration);
    if (media.proxyReady) line += L" · 성능용 편집본";
    return line;
}

void App::clearInspector() {
    for (HWND control : inspectorControls_) if (IsWindow(control)) DestroyWindow(control);
    inspectorControls_.clear();
    valueLabels_.clear();
}

HWND App::addInspectorLabel(const std::wstring& text, int x, int y, int width, int height, bool semibold) {
    HWND control = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        x, y, width, height, inspectorWindow_, nullptr, instance_, nullptr);
    setFont(control, semibold);
    inspectorControls_.push_back(control);
    return control;
}

HWND App::addInspectorButton(const std::wstring& text, int id, int x, int y, int width, int height) {
    HWND control = CreateWindowExW(0, L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, width, height, inspectorWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    setFont(control);
    applyDarkMode(control);
    inspectorControls_.push_back(control);
    return control;
}

HWND App::addInspectorTrack(const std::wstring& label, int id, int minimum, int maximum, int value, int y,
    const std::wstring& displayValue) {
    RECT area{};
    GetClientRect(inspectorWindow_, &area);
    const int width = std::max(250L, area.right);
    addInspectorLabel(label, 12, y, 88, 28);
    HWND track = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        98, y, std::max(90, width - 164), 28, inspectorWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(track, TBM_SETRANGEMIN, FALSE, minimum);
    SendMessageW(track, TBM_SETRANGEMAX, FALSE, maximum);
    SendMessageW(track, TBM_SETPOS, TRUE, std::clamp(value, minimum, maximum));
    SetWindowTheme(track, L"DarkMode_Explorer", nullptr);
    inspectorControls_.push_back(track);
    HWND valueLabel = addInspectorLabel(displayValue, width - 62, y, 52, 28);
    valueLabels_[id] = valueLabel;
    return track;
}

HWND App::addInspectorCombo(const std::wstring& label, int id, const std::vector<std::wstring>& items,
    int selected, int y) {
    RECT area{};
    GetClientRect(inspectorWindow_, &area);
    addInspectorLabel(label, 12, y, 88, 28);
    HWND combo = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        104, y, std::max(120L, area.right - 116), 260, inspectorWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    for (const auto& item : items) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    SendMessageW(combo, CB_SETCURSEL, std::clamp(selected, 0, std::max(0, static_cast<int>(items.size()) - 1)), 0);
    setFont(combo);
    applyDarkMode(combo);
    inspectorControls_.push_back(combo);
    return combo;
}

HWND App::addInspectorCheck(const std::wstring& label, int id, bool checked, int y) {
    HWND check = CreateWindowExW(0, L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        14, y, 250, 28, inspectorWindow_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    Button_SetCheck(check, checked ? BST_CHECKED : BST_UNCHECKED);
    setFont(check);
    applyDarkMode(check);
    inspectorControls_.push_back(check);
    return check;
}

HWND App::addInspectorEdit(const std::wstring& label, int id, const std::wstring& value, int y, int height, DWORD style) {
    RECT area{};
    GetClientRect(inspectorWindow_, &area);
    addInspectorLabel(label, 12, y, 88, 28);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOVSCROLL | style,
        104, y, std::max(120L, area.right - 116), height, inspectorWindow_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    setFont(edit);
    applyDarkMode(edit);
    inspectorControls_.push_back(edit);
    return edit;
}

void App::updateValueLabel(int propertyId, const std::wstring& value) {
    const auto iterator = valueLabels_.find(propertyId);
    if (iterator != valueLabels_.end()) SetControlText(iterator->second, value);
}

void App::buildInspector() {
    if (!inspectorWindow_) return;
    buildingInspector_ = true;
    clearInspector();
    RECT area{};
    GetClientRect(inspectorWindow_, &area);
    const int tabWidth = std::max(44, (static_cast<int>(area.right) - 14) / 6);
    const std::array<std::pair<const wchar_t*, int>, 6> tabs{{
        {L"기본색", ID_TAB_COLOR_BASIC}, {L"정밀색", ID_TAB_COLOR_PRECISE}, {L"클립", ID_TAB_CLIP},
        {L"오디오", ID_TAB_AUDIO}, {L"텍스트", ID_TAB_TEXT}, {L"출력", ID_TAB_EXPORT}
    }};
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        std::wstring label = tabs[static_cast<std::size_t>(i)].first;
        if (i == inspectorTab_) label = L"● " + label;
        addInspectorButton(label, tabs[static_cast<std::size_t>(i)].second, 7 + i * tabWidth, 8, tabWidth - 2, 30);
    }

    Clip* clip = selectedClip();
    if (!clip && inspectorTab_ != 5) {
        addInspectorLabel(L"속성", 16, 54, area.right - 32, 28, true);
        addInspectorLabel(L"타임라인에서 클립을 선택하면\n여기에 편집 속성이 표시됩니다.", 24, 104, area.right - 48, 72);
        addInspectorButton(L"미디어 가져오기", ID_TOOL_IMPORT, 24, 190, area.right - 48, 34);
        buildingInspector_ = false;
        InvalidateRect(inspectorWindow_, nullptr, TRUE);
        return;
    }

    auto number = [](double value, int precision = 1) {
        std::wostringstream stream;
        stream << std::fixed << std::setprecision(precision) << value;
        return stream.str();
    };
    int y = 52;
    if (inspectorTab_ == 0 && clip) {
        addInspectorLabel(L"시네마틱 색보정", 14, y, area.right - 28, 28, true); y += 34;
        addInspectorTrack(L"노출", ID_PROP_EXPOSURE, -500, 500, static_cast<int>(clip->color.exposure * 100), y, number(clip->color.exposure)); y += 32;
        addInspectorTrack(L"대비", ID_PROP_CONTRAST, -100, 100, static_cast<int>(clip->color.contrast), y, number(clip->color.contrast, 0)); y += 32;
        addInspectorTrack(L"하이라이트", ID_PROP_HIGHLIGHTS, -100, 100, static_cast<int>(clip->color.highlights), y, number(clip->color.highlights, 0)); y += 32;
        addInspectorTrack(L"그림자", ID_PROP_SHADOWS, -100, 100, static_cast<int>(clip->color.shadows), y, number(clip->color.shadows, 0)); y += 32;
        addInspectorTrack(L"화이트", ID_PROP_WHITES, -100, 100, static_cast<int>(clip->color.whites), y, number(clip->color.whites, 0)); y += 32;
        addInspectorTrack(L"블랙", ID_PROP_BLACKS, -100, 100, static_cast<int>(clip->color.blacks), y, number(clip->color.blacks, 0)); y += 32;
        addInspectorTrack(L"색온도", ID_PROP_TEMPERATURE, -100, 100, static_cast<int>(clip->color.temperature), y, number(clip->color.temperature, 0)); y += 32;
        addInspectorTrack(L"틴트", ID_PROP_TINT, -100, 100, static_cast<int>(clip->color.tint), y, number(clip->color.tint, 0)); y += 32;
        addInspectorTrack(L"채도", ID_PROP_SATURATION, 0, 200, static_cast<int>(clip->color.saturation), y, number(clip->color.saturation, 0)); y += 35;
        addInspectorCombo(L"Film Look", ID_PROP_LOOK,
            {L"없음", L"Cinema Neutral", L"Warm Film", L"Cold Film", L"Bleach", L"Soft Portrait", L"Night", L"Vintage"},
            static_cast<int>(clip->color.look), y); y += 36;
        addInspectorTrack(L"룩 강도", ID_PROP_LOOK_STRENGTH, 0, 100, static_cast<int>(clip->color.lookStrength), y, number(clip->color.lookStrength, 0) + L"%"); y += 32;
        addInspectorTrack(L"필름 그레인", ID_PROP_GRAIN, 0, 100, static_cast<int>(clip->color.grain), y, number(clip->color.grain, 0) + L"%"); y += 32;
        addInspectorTrack(L"비네트", ID_PROP_VIGNETTE, 0, 100, static_cast<int>(clip->color.vignette), y, number(clip->color.vignette, 0) + L"%"); y += 32;
        addInspectorTrack(L"레터박스", ID_PROP_LETTERBOX, 0, 200, static_cast<int>(clip->color.letterbox * 10), y, number(clip->color.letterbox) + L"%");
    } else if (inspectorTab_ == 1 && clip) {
        addInspectorLabel(L"커브 · 색상 휠", 14, y, area.right - 28, 28, true); y += 36;
        addInspectorLabel(L"RGB Curve", 14, y, area.right - 28, 24, true); y += 27;
        addInspectorTrack(L"그림자 점", ID_PROP_CURVE_SHADOWS, -100, 100, static_cast<int>(clip->color.curveShadows), y, number(clip->color.curveShadows, 0)); y += 34;
        addInspectorTrack(L"중간 점", ID_PROP_CURVE_MIDTONES, -100, 100, static_cast<int>(clip->color.curveMidtones), y, number(clip->color.curveMidtones, 0)); y += 34;
        addInspectorTrack(L"밝은 점", ID_PROP_CURVE_HIGHLIGHTS, -100, 100, static_cast<int>(clip->color.curveHighlights), y, number(clip->color.curveHighlights, 0)); y += 38;
        const auto wheel = [&](const wchar_t* title, int baseId, const std::array<double, 3>& values) {
            addInspectorLabel(title, 14, y, area.right - 28, 24, true); y += 25;
            addInspectorTrack(L"R", baseId, -100, 100, static_cast<int>(values[0]), y, number(values[0], 0)); y += 30;
            addInspectorTrack(L"G", baseId + 1, -100, 100, static_cast<int>(values[1]), y, number(values[1], 0)); y += 30;
            addInspectorTrack(L"B", baseId + 2, -100, 100, static_cast<int>(values[2]), y, number(values[2], 0)); y += 34;
        };
        wheel(L"Shadow Wheel", ID_PROP_SHADOW_R, clip->color.shadowWheel);
        wheel(L"Midtone Wheel", ID_PROP_MID_R, clip->color.midtoneWheel);
        wheel(L"Highlight Wheel", ID_PROP_HIGH_R, clip->color.highlightWheel);
    } else if (inspectorTab_ == 2 && clip) {
        const MediaItem* media = project_.findMedia(clip->mediaId);
        addInspectorLabel(L"클립", 14, y, area.right - 28, 28, true); y += 38;
        if (media) {
            addInspectorLabel(Utf8ToWide(media->displayName), 16, y, area.right - 32, 26, true); y += 28;
            addInspectorLabel(mediaDisplayLine(*media), 16, y, area.right - 32, 24); y += 36;
        }
        addInspectorTrack(L"재생 속도", ID_PROP_SPEED, 10, 800, static_cast<int>(clip->speed * 100), y, number(clip->speed, 2) + L"×"); y += 42;
        addInspectorLabel(L"원본 구간  " + formatTime(clip->inPoint) + L"  →  " + formatTime(clip->outPoint),
            14, y, area.right - 28, 28); y += 40;
        const int half = (area.right - 42) / 2;
        addInspectorButton(L"현재 위치 분할", ID_PROP_SPLIT, 14, y, half, 34);
        addInspectorButton(L"클립 복제", ID_PROP_DUPLICATE, 28 + half, y, half, 34); y += 42;
        addInspectorButton(L"앞 리플 트림", ID_PROP_TRIM_HEAD, 14, y, half, 34);
        addInspectorButton(L"뒤 리플 트림", ID_PROP_TRIM_TAIL, 28 + half, y, half, 34); y += 42;
        addInspectorButton(L"리플 삭제", ID_PROP_DELETE, 14, y, area.right - 28, 34);
    } else if (inspectorTab_ == 3 && clip) {
        addInspectorLabel(L"오디오", 14, y, area.right - 28, 28, true); y += 40;
        addInspectorTrack(L"볼륨", ID_PROP_VOLUME, -600, 240, static_cast<int>(clip->audio.volumeDb * 10), y, number(clip->audio.volumeDb) + L" dB"); y += 36;
        addInspectorTrack(L"페이드 인", ID_PROP_FADE_IN, 0, static_cast<int>(clip->timelineDuration() * 100), static_cast<int>(clip->audio.fadeIn * 100), y, number(clip->audio.fadeIn) + L"초"); y += 36;
        addInspectorTrack(L"페이드 아웃", ID_PROP_FADE_OUT, 0, static_cast<int>(clip->timelineDuration() * 100), static_cast<int>(clip->audio.fadeOut * 100), y, number(clip->audio.fadeOut) + L"초"); y += 44;
        addInspectorLabel(L"3밴드 EQ", 14, y, area.right - 28, 24, true); y += 28;
        addInspectorTrack(L"저음 120 Hz", ID_PROP_EQ_LOW, -180, 180, static_cast<int>(clip->audio.eqLow * 10), y, number(clip->audio.eqLow) + L" dB"); y += 36;
        addInspectorTrack(L"중음 1 kHz", ID_PROP_EQ_MID, -180, 180, static_cast<int>(clip->audio.eqMid * 10), y, number(clip->audio.eqMid) + L" dB"); y += 36;
        addInspectorTrack(L"고음 6 kHz", ID_PROP_EQ_HIGH, -180, 180, static_cast<int>(clip->audio.eqHigh * 10), y, number(clip->audio.eqHigh) + L" dB"); y += 42;
        addInspectorCheck(L"음소거", ID_PROP_MUTE, clip->audio.muted, y); y += 32;
        addInspectorCheck(L"-16 LUFS 자동 노멀라이즈", ID_PROP_NORMALIZE, clip->audio.normalize, y);
    } else if (inspectorTab_ == 4 && clip) {
        addInspectorLabel(L"텍스트", 14, y, area.right - 28, 28, true); y += 38;
        if (clip->texts.empty()) {
            addInspectorLabel(L"이 클립에는 텍스트가 없습니다.", 18, y, area.right - 36, 32); y += 42;
            addInspectorButton(L"텍스트 추가", ID_TEXT_ADD, 18, y, area.right - 36, 36);
        } else {
            TextOverlay& text = clip->texts.front();
            addInspectorEdit(L"내용", ID_TEXT_CONTENT, Utf8ToWide(text.text), y, 58, ES_MULTILINE | ES_WANTRETURN); y += 66;
            int fontIndex = text.font == "Segoe UI" ? 1 : text.font == "Arial" ? 2 : text.font == "Georgia" ? 3 : 0;
            addInspectorCombo(L"글꼴", ID_TEXT_FONT, {L"맑은 고딕", L"Segoe UI", L"Arial", L"Georgia"}, fontIndex, y); y += 38;
            addInspectorCombo(L"위치", ID_TEXT_POSITION, {L"위", L"가운데", L"아래", L"사용자 지정"}, static_cast<int>(text.position), y); y += 38;
            addInspectorTrack(L"크기", ID_TEXT_SIZE, 8, 240, static_cast<int>(text.fontSize), y, number(text.fontSize, 0)); y += 34;
            addInspectorTrack(L"불투명도", ID_TEXT_OPACITY, 0, 100, static_cast<int>(text.opacity * 100), y, number(text.opacity * 100, 0) + L"%"); y += 34;
            addInspectorTrack(L"페이드 인", ID_TEXT_FADE_IN, 0, static_cast<int>(clip->timelineDuration() * 100), static_cast<int>(text.fadeIn * 100), y, number(text.fadeIn) + L"초"); y += 34;
            addInspectorTrack(L"페이드 아웃", ID_TEXT_FADE_OUT, 0, static_cast<int>(clip->timelineDuration() * 100), static_cast<int>(text.fadeOut * 100), y, number(text.fadeOut) + L"초"); y += 42;
            addInspectorButton(L"텍스트 삭제", ID_TEXT_DELETE, 18, y, area.right - 36, 34);
        }
    } else {
        addInspectorLabel(L"영상 출력", 14, y, area.right - 28, 28, true); y += 42;
        int resolution = project_.output.width >= 3000 ? 2 : project_.output.width <= 1280 ? 0 : 1;
        addInspectorCombo(L"해상도", ID_OUTPUT_RESOLUTION, {L"720p", L"1080p", L"4K UHD"}, resolution, y); y += 40;
        addInspectorCombo(L"코덱", ID_OUTPUT_CODEC, {L"H.264 · 높은 호환성", L"H.265 · 작은 용량"},
            project_.output.codec == VideoCodec::H265 ? 1 : 0, y); y += 40;
        int quality = project_.output.quality == ExportQuality::Compact ? 0 : project_.output.quality == ExportQuality::Standard ? 1 : 2;
        addInspectorCombo(L"품질", ID_OUTPUT_QUALITY, {L"작은 용량", L"표준", L"고품질"}, quality, y); y += 44;
        addInspectorCheck(L"오디오 포함", ID_OUTPUT_AUDIO, project_.output.preserveAudio, y); y += 44;
        addInspectorLabel(L"최종 출력은 항상 원본 미디어를 사용합니다.\n성능용 편집본은 결과물에 들어가지 않습니다.",
            18, y, area.right - 36, 58); y += 70;
        addInspectorButton(exporting_ ? L"출력 중…" : L"영상 출력", ID_OUTPUT_EXPORT, 18, y, area.right - 36, 40);
    }
    buildingInspector_ = false;
    InvalidateRect(inspectorWindow_, nullptr, TRUE);
}

void App::switchInspectorTab(int tab) {
    inspectorTab_ = std::clamp(tab, 0, 5);
    buildInspector();
}

void App::scheduleAccuratePreview() {
    if (!selectedClip()) return;
    if (player_.playing()) {
        player_.pause();
        SetControlText(playButton_, L"▶  재생");
    }
    SetTimer(mainWindow_, TIMER_GRADE_DEBOUNCE, 180, nullptr);
    setStatus(L"색감 미리보기 갱신 중… UI는 계속 사용할 수 있습니다.");
}

void App::renderAccuratePreview() {
    const Clip* clip = selectedClip();
    if (!clip) return;
    const MediaItem* media = project_.findMedia(clip->mediaId);
    if (!media) return;
    const double position = player_.ready() ? player_.position() : clip->inPoint;
    const std::uint64_t generation = ++previewGeneration_;
    const std::filesystem::path output = cacheDirectory_ /
        (L"grade-preview-" + std::to_wstring(clip->id) + L"-" + std::to_wstring(generation) + L".bmp");
    const std::filesystem::path executable = ffmpegPath_;
    const AdaptiveProfile profile = ChooseAdaptiveProfile(project_.performanceMode, logicalProcessors_, physicalMemoryMb_, media);
    const HWND target = mainWindow_;
    const MediaItem mediaCopy = *media;
    const Clip clipCopy = *clip;
    previewWorker_.submit([executable, target, mediaCopy, clipCopy, position, output, profile, generation] {
        auto completion = std::make_unique<PreviewCompletion>();
        completion->clipId = clipCopy.id;
        completion->generation = generation;
        completion->path = output;
        const ProcessResult result = RunHiddenProcess(executable, BuildPreviewStillArguments(
            mediaCopy, clipCopy, position, output, profile.previewWidth, profile.previewHeight, false));
        completion->success = result.started && result.exitCode == 0 && std::filesystem::exists(output);
        completion->error = result.error.empty() ? Utf8ToWide(result.output) : result.error;
        PostMessageW(target, WM_FILLEMA_PREVIEW_READY, 0, reinterpret_cast<LPARAM>(completion.release()));
    });
}

void App::clearPreviewBitmap() {
    if (previewBitmap_) {
        DeleteObject(previewBitmap_);
        previewBitmap_ = nullptr;
    }
    showAccurateStill_ = false;
}

void App::exportDialog() {
    if (exporting_) {
        setStatus(L"이미 영상을 출력하고 있습니다. 편집 화면은 계속 사용할 수 있습니다.");
        return;
    }
    if (project_.timeline.empty()) {
        MessageBoxW(mainWindow_, L"타임라인이 비어 있습니다. 먼저 클립을 추가해 주세요.", L"Fillema", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::array<wchar_t, 32768> file{};
    const std::wstring suggested = Utf8ToWide(project_.name) + L".mp4";
    wcsncpy_s(file.data(), file.size(), suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = mainWindow_;
    dialog.lpstrFilter = L"MP4 영상 (*.mp4)\0*.mp4\0\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrDefExt = L"mp4";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) return;
    beginExport(AddExtensionIfMissing(file.data(), L".mp4"));
}

void App::beginExport(const std::filesystem::path& outputPath) {
    if (exporting_) return;
    Project snapshot = project_;
    const auto issues = ValidateProject(snapshot);
    for (const auto& issue : issues) {
        if (issue.severity == ValidationIssue::Severity::Error) {
            MessageBoxW(mainWindow_, Utf8ToWide(issue.message).c_str(), L"Fillema", MB_OK | MB_ICONERROR);
            return;
        }
    }
    exporting_ = true;
    EnableWindow(GetDlgItem(mainWindow_, ID_TOOL_EXPORT), FALSE);
    buildInspector();
    setStatus(L"영상 출력 중 · 렌더링은 백그라운드에서 실행되며 UI는 계속 반응합니다.");

    const std::filesystem::path script = cacheDirectory_ / (L"export-" + std::to_wstring(GetTickCount64()) + L".filters");
    const std::filesystem::path executable = ffmpegPath_;
    const HWND target = mainWindow_;
    exportWorker_.submit([snapshot = std::move(snapshot), script, outputPath, executable, target] {
        auto completion = std::make_unique<ExportCompletion>();
        completion->output = outputPath;
        try {
            const ExportPlan plan = BuildExportPlan(snapshot, script, outputPath);
            if (!plan.warnings.empty()) {
                completion->error = Utf8ToWide(plan.warnings.front());
            } else {
                std::ofstream stream(script, std::ios::binary | std::ios::trunc);
                stream.write(plan.filterComplex.data(), static_cast<std::streamsize>(plan.filterComplex.size()));
                stream.close();
                if (!stream) {
                    completion->error = L"렌더링 필터 파일을 만들지 못했습니다.";
                } else {
                    const ProcessResult result = RunHiddenProcess(executable, plan.arguments);
                    completion->success = result.started && result.exitCode == 0 && std::filesystem::exists(outputPath);
                    if (!completion->success) {
                        completion->error = result.error.empty() ? Utf8ToWide(result.output) : result.error;
                        if (completion->error.size() > 5000) completion->error = completion->error.substr(completion->error.size() - 5000);
                    }
                }
            }
        } catch (const std::exception& exception) {
            completion->error = ErrorFromException(exception);
        }
        std::error_code error;
        std::filesystem::remove(script, error);
        PostMessageW(target, WM_FILLEMA_EXPORT_COMPLETE, 0, reinterpret_cast<LPARAM>(completion.release()));
    });
}

void App::showCommandBar() {
    if (!commandPanel_) return;
    ShowWindow(commandPanel_, SW_SHOW);
    BringWindowToTop(commandPanel_);
    SetWindowTextW(commandEdit_, L"");
    filterCommands();
    SetFocus(commandEdit_);
}

void App::hideCommandBar() {
    ShowWindow(commandPanel_, SW_HIDE);
    SetFocus(mainWindow_);
}

void App::filterCommands() {
    if (!commandList_) return;
    std::wstring query = WindowText(commandEdit_);
    if (!query.empty()) CharLowerBuffW(query.data(), static_cast<DWORD>(query.size()));
    SendMessageW(commandList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(commandList_, LB_RESETCONTENT, 0, 0);
    for (const auto& [label, action] : commands_) {
        std::wstring searchable = label;
        if (!searchable.empty()) CharLowerBuffW(searchable.data(), static_cast<DWORD>(searchable.size()));
        if (!query.empty() && searchable.find(query) == std::wstring::npos) continue;
        const LRESULT index = SendMessageW(commandList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (index != LB_ERR) SendMessageW(commandList_, LB_SETITEMDATA, index, action);
    }
    if (SendMessageW(commandList_, LB_GETCOUNT, 0, 0) > 0) SendMessageW(commandList_, LB_SETCURSEL, 0, 0);
    SendMessageW(commandList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(commandList_, nullptr, TRUE);
}

void App::executeCommandSelection() {
    const LRESULT selected = SendMessageW(commandList_, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR) return;
    const int action = static_cast<int>(SendMessageW(commandList_, LB_GETITEMDATA, selected, 0));
    hideCommandBar();
    if (action == 10001 || action == 10002) {
        recordUndo();
        if (action == 10002) { project_.output.width = 3840; project_.output.height = 2160; }
        else { project_.output.width = 1920; project_.output.height = 1080; }
        markDirty();
        switchInspectorTab(5);
        setStatus(action == 10002 ? L"출력을 4K UHD로 설정했습니다." : L"출력을 1080p로 설정했습니다.");
        return;
    }
    if (action == ID_TAB_COLOR_BASIC || action == ID_TAB_COLOR_PRECISE) {
        switchInspectorTab(action == ID_TAB_COLOR_BASIC ? 0 : 1);
        return;
    }
    if (action == ID_PROP_EXPOSURE) {
        switchInspectorTab(0);
        if (HWND track = GetDlgItem(inspectorWindow_, ID_PROP_EXPOSURE)) SetFocus(track);
        return;
    }
    if (action == ID_TEXT_ADD) {
        handleCommand(ID_TEXT_ADD, BN_CLICKED, nullptr);
        switchInspectorTab(4);
        return;
    }
    handleCommand(action, 0, nullptr);
}

void App::setFullscreen(bool enabled) {
    if (fullscreen_ == enabled) return;
    fullscreen_ = enabled;
    const auto showEditingUi = [&](int command) {
        for (HWND control : toolbarControls_) ShowWindow(control, command);
        for (HWND control : {mediaHeader_, mediaList_, timelineWindow_, inspectorWindow_, statusWindow_, playButton_, timeLabel_}) {
            ShowWindow(control, command);
        }
    };
    if (enabled) {
        hideCommandBar();
        savedStyle_ = GetWindowLongPtrW(mainWindow_, GWL_STYLE);
        GetWindowPlacement(mainWindow_, &savedPlacement_);
        showEditingUi(SW_HIDE);
        SetMenu(mainWindow_, nullptr);
        SetWindowLongPtrW(mainWindow_, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(mainWindow_, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowPos(mainWindow_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(mainWindow_, GWL_STYLE, savedStyle_);
        SetMenu(mainWindow_, mainMenu_);
        showEditingUi(SW_SHOW);
        SetWindowPlacement(mainWindow_, &savedPlacement_);
        SetWindowPos(mainWindow_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    RECT area{};
    GetClientRect(mainWindow_, &area);
    layout(area.right, area.bottom);
}

} // namespace fillema
