#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"FebiusFillemaWindow";
constexpr wchar_t kAppName[] = L"Febius Fillema";
constexpr UINT_PTR kPlaybackTimer = 1;
constexpr UINT kPlaybackTimerMs = 33;

constexpr int ID_FILE_NEW = 1001;
constexpr int ID_FILE_OPEN = 1002;
constexpr int ID_FILE_SAVE = 1003;
constexpr int ID_FILE_SAVE_AS = 1004;
constexpr int ID_FILE_IMPORT = 1005;
constexpr int ID_FILE_EXIT = 1006;
constexpr int ID_EDIT_SPLIT = 1101;
constexpr int ID_EDIT_DELETE = 1102;
constexpr int ID_PLAY_TOGGLE = 1201;

constexpr COLORREF kBackground = RGB(21, 22, 24);
constexpr COLORREF kPanel = RGB(31, 33, 36);
constexpr COLORREF kPanelAlt = RGB(26, 28, 31);
constexpr COLORREF kBorder = RGB(55, 58, 63);
constexpr COLORREF kText = RGB(232, 233, 235);
constexpr COLORREF kMuted = RGB(154, 158, 166);
constexpr COLORREF kAccent = RGB(124, 104, 238);
constexpr COLORREF kClip = RGB(70, 77, 100);
constexpr COLORREF kSelected = RGB(109, 95, 210);

struct MediaItem {
    std::wstring path;
    std::wstring name;
};

struct Clip {
    std::size_t mediaIndex = 0;
    double startSeconds = 0.0;
    double durationSeconds = 10.0;
};

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Frame(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawLabel(HDC dc, const wchar_t* value, RECT rect, COLORREF color,
               UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value, -1, &rect, flags);
}

std::wstring FormatTime(double seconds) {
    const int totalMs = static_cast<int>(std::max(0.0, seconds) * 1000.0 + 0.5);
    const int minutes = totalMs / 60000;
    const int secs = (totalMs / 1000) % 60;
    const int millis = totalMs % 1000;
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%02d:%02d.%03d", minutes, secs, millis);
    return buffer;
}

class Project {
public:
    void Clear() {
        media_.clear();
        clips_.clear();
        filePath_.clear();
        playheadSeconds_ = 0.0;
    }

    std::size_t AddMedia(const std::wstring& path) {
        const auto found = std::find_if(media_.begin(), media_.end(), [&](const MediaItem& item) {
            return _wcsicmp(item.path.c_str(), path.c_str()) == 0;
        });
        if (found != media_.end()) {
            return static_cast<std::size_t>(std::distance(media_.begin(), found));
        }

        media_.push_back({path, std::filesystem::path(path).filename().wstring()});
        return media_.size() - 1;
    }

    std::size_t AddClip(std::size_t mediaIndex, double startSeconds, double durationSeconds) {
        clips_.push_back({mediaIndex, std::max(0.0, startSeconds), std::max(0.05, durationSeconds)});
        return clips_.size() - 1;
    }

    std::optional<std::size_t> Split(std::size_t clipIndex, double atSeconds) {
        if (clipIndex >= clips_.size()) return std::nullopt;

        Clip& source = clips_[clipIndex];
        const double local = atSeconds - source.startSeconds;
        constexpr double kMinimumPiece = 0.05;
        if (local <= kMinimumPiece || local >= source.durationSeconds - kMinimumPiece) {
            return std::nullopt;
        }

        Clip second = source;
        second.startSeconds = atSeconds;
        second.durationSeconds = source.durationSeconds - local;
        source.durationSeconds = local;
        clips_.insert(clips_.begin() + static_cast<std::ptrdiff_t>(clipIndex + 1), second);
        return clipIndex + 1;
    }

    bool Remove(std::size_t clipIndex) {
        if (clipIndex >= clips_.size()) return false;
        clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(clipIndex));
        return true;
    }

    bool Save(const std::wstring& path) const {
        std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        if (!out) return false;

        out << "FILLEMA 1\n";
        out << std::setprecision(15);
        out << "PLAYHEAD " << playheadSeconds_ << '\n';
        for (const auto& item : media_) {
            out << "MEDIA " << std::quoted(ToUtf8(item.path)) << '\n';
        }
        for (const auto& clip : clips_) {
            out << "CLIP " << clip.mediaIndex << ' ' << clip.startSeconds << ' ' << clip.durationSeconds << '\n';
        }
        return static_cast<bool>(out);
    }

    bool Load(const std::wstring& path) {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        if (!in) return false;

        std::string signature;
        int version = 0;
        if (!(in >> signature >> version) || signature != "FILLEMA" || version != 1) {
            return false;
        }

        Project loaded;
        std::string token;
        while (in >> token) {
            if (token == "PLAYHEAD") {
                in >> loaded.playheadSeconds_;
            } else if (token == "MEDIA") {
                std::string utf8Path;
                in >> std::quoted(utf8Path);
                loaded.AddMedia(FromUtf8(utf8Path));
            } else if (token == "CLIP") {
                Clip clip;
                in >> clip.mediaIndex >> clip.startSeconds >> clip.durationSeconds;
                if (!in) return false;
                loaded.clips_.push_back(clip);
            } else {
                std::string ignored;
                std::getline(in, ignored);
            }
        }

        loaded.clips_.erase(
            std::remove_if(loaded.clips_.begin(), loaded.clips_.end(), [&](const Clip& clip) {
                return clip.mediaIndex >= loaded.media_.size() || clip.durationSeconds <= 0.0;
            }),
            loaded.clips_.end());

        loaded.filePath_ = path;
        *this = std::move(loaded);
        return true;
    }

    double EndSeconds() const {
        double end = 0.0;
        for (const auto& clip : clips_) {
            end = std::max(end, clip.startSeconds + clip.durationSeconds);
        }
        return end;
    }

    const std::vector<MediaItem>& Media() const { return media_; }
    const std::vector<Clip>& Clips() const { return clips_; }
    std::vector<Clip>& Clips() { return clips_; }

    double PlayheadSeconds() const { return playheadSeconds_; }
    void SetPlayheadSeconds(double value) { playheadSeconds_ = std::max(0.0, value); }

    const std::wstring& FilePath() const { return filePath_; }
    void SetFilePath(std::wstring path) { filePath_ = std::move(path); }

private:
    std::vector<MediaItem> media_;
    std::vector<Clip> clips_;
    std::wstring filePath_;
    double playheadSeconds_ = 0.0;
};

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    ~App() {
        if (font_) DeleteObject(font_);
        if (fontBold_) DeleteObject(fontBold_);
    }

    bool Initialize(int showCommand) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClass;

        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        font_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        fontBold_ = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        hwnd_ = CreateWindowExW(0, kWindowClass, kAppName,
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900,
                                nullptr, nullptr, instance_, this);
        if (!hwnd_) return false;

        CreateMenus();
        ShowWindow(hwnd_, showCommand);
        UpdateWindow(hwnd_);
        UpdateTitle();
        return true;
    }

    int Run() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    struct Layout {
        RECT media{};
        RECT preview{};
        RECT inspector{};
        RECT timeline{};
        RECT status{};
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        App* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<App*>(create->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->HandleMessage(message, wParam, lParam)
                    : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case ID_FILE_NEW: NewProject(); return 0;
            case ID_FILE_OPEN: OpenProject(); return 0;
            case ID_FILE_SAVE: SaveProject(false); return 0;
            case ID_FILE_SAVE_AS: SaveProject(true); return 0;
            case ID_FILE_IMPORT: ImportMedia(); return 0;
            case ID_FILE_EXIT: DestroyWindow(hwnd_); return 0;
            case ID_EDIT_SPLIT: SplitSelected(); return 0;
            case ID_EDIT_DELETE: DeleteSelected(); return 0;
            case ID_PLAY_TOGGLE: TogglePlayback(); return 0;
            default: break;
            }
            break;

        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                if (wParam == 'S') { SaveProject(false); return 0; }
                if (wParam == 'O') { OpenProject(); return 0; }
                if (wParam == 'I') { ImportMedia(); return 0; }
                if (wParam == 'N') { NewProject(); return 0; }
            }
            if (wParam == VK_SPACE) { TogglePlayback(); return 0; }
            if (wParam == 'S') { SplitSelected(); return 0; }
            if (wParam == VK_DELETE) { DeleteSelected(); return 0; }
            break;

        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const Layout layout = CalculateLayout();
            POINT point{x, y};
            if (PtInRect(&layout.timeline, point)) {
                selectedClip_ = ClipAtPoint(x, y);
                project_.SetPlayheadSeconds(TimelineSecondsAtX(x));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == kPlaybackTimer && playing_) {
                double next = project_.PlayheadSeconds() + static_cast<double>(kPlaybackTimerMs) / 1000.0;
                const double end = project_.EndSeconds();
                if (end > 0.0 && next >= end) {
                    next = end;
                    playing_ = false;
                    KillTimer(hwnd_, kPlaybackTimer);
                }
                project_.SetPlayheadSeconds(next);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_PAINT:
            Paint();
            return 0;

        case WM_SIZE:
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_DESTROY:
            KillTimer(hwnd_, kPlaybackTimer);
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    void CreateMenus() {
        HMENU menu = CreateMenu();
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, ID_FILE_NEW, L"새 프로젝트\tCtrl+N");
        AppendMenuW(file, MF_STRING, ID_FILE_OPEN, L"프로젝트 열기...\tCtrl+O");
        AppendMenuW(file, MF_STRING, ID_FILE_SAVE, L"저장\tCtrl+S");
        AppendMenuW(file, MF_STRING, ID_FILE_SAVE_AS, L"다른 이름으로 저장...");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, ID_FILE_IMPORT, L"미디어 가져오기...\tCtrl+I");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, ID_FILE_EXIT, L"끝내기");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"파일");

        HMENU edit = CreatePopupMenu();
        AppendMenuW(edit, MF_STRING, ID_EDIT_SPLIT, L"플레이헤드에서 나누기\tS");
        AppendMenuW(edit, MF_STRING, ID_EDIT_DELETE, L"선택 클립 삭제\tDelete");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"편집");

        HMENU playback = CreatePopupMenu();
        AppendMenuW(playback, MF_STRING, ID_PLAY_TOGGLE, L"재생 / 정지\tSpace");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(playback), L"재생");
        SetMenu(hwnd_, menu);
    }

    Layout CalculateLayout() const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = std::max(1, static_cast<int>(client.right - client.left));
        const int height = std::max(1, static_cast<int>(client.bottom - client.top));

        const int statusHeight = 26;
        const int timelineHeight = std::clamp(height * 34 / 100, 190, 330);
        const int topBottom = std::max(0, height - timelineHeight - statusHeight);
        const int mediaWidth = std::clamp(width * 18 / 100, 210, 300);
        const int inspectorWidth = std::clamp(width * 20 / 100, 250, 340);

        Layout l;
        l.media = {0, 0, mediaWidth, topBottom};
        l.preview = {mediaWidth + 1, 0, std::max(mediaWidth + 2, width - inspectorWidth - 1), topBottom};
        l.inspector = {std::max(mediaWidth + 2, width - inspectorWidth), 0, width, topBottom};
        l.timeline = {0, topBottom + 1, width, std::max(topBottom + 2, height - statusHeight)};
        l.status = {0, std::max(0, height - statusHeight), width, height};
        return l;
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        SelectObject(dc, font_);

        RECT client{};
        GetClientRect(hwnd_, &client);
        Fill(dc, client, kBackground);

        const Layout l = CalculateLayout();
        PaintMedia(dc, l.media);
        PaintPreview(dc, l.preview);
        PaintInspector(dc, l.inspector);
        PaintTimeline(dc, l.timeline);
        PaintStatus(dc, l.status);
        EndPaint(hwnd_, &ps);
    }

    void PaintPanel(HDC dc, const RECT& rect, COLORREF color, const wchar_t* title) {
        Fill(dc, rect, color);
        Frame(dc, rect, kBorder);
        RECT titleRect = rect;
        titleRect.left += 12;
        titleRect.top += 7;
        titleRect.bottom = titleRect.top + 24;
        SelectObject(dc, fontBold_);
        DrawLabel(dc, title, titleRect, kText);
        SelectObject(dc, font_);
    }

    void PaintMedia(HDC dc, const RECT& rect) {
        PaintPanel(dc, rect, kPanel, L"미디어");
        int y = rect.top + 40;
        if (project_.Media().empty()) {
            RECT empty{rect.left + 12, y, rect.right - 12, y + 56};
            DrawLabel(dc, L"Ctrl+I로 영상을 가져오세요.", empty, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
            return;
        }

        for (const auto& item : project_.Media()) {
            RECT row{rect.left + 8, y, rect.right - 8, y + 32};
            Fill(dc, row, kPanelAlt);
            row.left += 8;
            row.right -= 8;
            DrawLabel(dc, item.name.c_str(), row, kText);
            y += 36;
            if (y > rect.bottom - 32) break;
        }
    }

    void PaintPreview(HDC dc, const RECT& rect) {
        PaintPanel(dc, rect, RGB(16, 17, 19), L"미리보기");
        RECT center = rect;
        center.top += 34;
        center.left += 24;
        center.right -= 24;
        DrawLabel(dc,
                  project_.Media().empty() ? L"미디어를 가져오면 이곳에서 영상을 확인합니다." : L"영상 엔진 연결 준비됨 · Fillema 0.1",
                  center, kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void PaintInspector(HDC dc, const RECT& rect) {
        PaintPanel(dc, rect, kPanel, L"속성");
        int y = rect.top + 42;
        if (!selectedClip_ || *selectedClip_ >= project_.Clips().size()) {
            RECT empty{rect.left + 12, y, rect.right - 12, y + 48};
            DrawLabel(dc, L"타임라인에서 클립을 선택하세요.", empty, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
            return;
        }

        const Clip& clip = project_.Clips()[*selectedClip_];
        const MediaItem& media = project_.Media()[clip.mediaIndex];
        const std::wstring start = L"시작    " + FormatTime(clip.startSeconds);
        const std::wstring duration = L"길이    " + FormatTime(clip.durationSeconds);

        RECT row{rect.left + 12, y, rect.right - 12, y + 24};
        DrawLabel(dc, media.name.c_str(), row, kText);
        row.top += 34; row.bottom += 34;
        DrawLabel(dc, start.c_str(), row, kMuted);
        row.top += 28; row.bottom += 28;
        DrawLabel(dc, duration.c_str(), row, kMuted);
        row.top += 42; row.bottom += 42;
        DrawLabel(dc, L"Color · Audio · Transform", row, kText);
        row.top += 26; row.bottom += 52;
        DrawLabel(dc, L"다음 단계에서 경량 효과 엔진을 연결합니다.", row, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    double VisibleTimelineSeconds() const {
        return std::max(60.0, std::ceil(project_.EndSeconds() / 10.0) * 10.0);
    }

    void PaintTimeline(HDC dc, const RECT& rect) {
        PaintPanel(dc, rect, kPanelAlt, L"타임라인");
        const int leftGutter = 72;
        const int rulerTop = rect.top + 34;
        const int trackTop = rulerTop + 32;
        const int trackBottom = std::min(rect.bottom - 14, trackTop + 72);
        const int contentLeft = rect.left + leftGutter;
        const int contentRight = rect.right - 12;
        const int contentWidth = std::max(1, contentRight - contentLeft);
        const double visible = VisibleTimelineSeconds();

        RECT trackLabel{rect.left + 10, trackTop, contentLeft - 8, trackBottom};
        DrawLabel(dc, L"V1", trackLabel, kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT track{contentLeft, trackTop, contentRight, trackBottom};
        Fill(dc, track, RGB(23, 25, 28));

        for (int second = 0; second <= static_cast<int>(visible); second += 10) {
            const int x = contentLeft + static_cast<int>((static_cast<double>(second) / visible) * contentWidth);
            HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, x, rulerTop + 18, nullptr);
            LineTo(dc, x, trackBottom);
            SelectObject(dc, old);
            DeleteObject(pen);

            wchar_t label[32]{};
            swprintf_s(label, L"%ds", second);
            RECT labelRect{x + 4, rulerTop, x + 50, rulerTop + 20};
            DrawLabel(dc, label, labelRect, kMuted);
        }

        for (std::size_t i = 0; i < project_.Clips().size(); ++i) {
            const Clip& clip = project_.Clips()[i];
            const int x1 = contentLeft + static_cast<int>((clip.startSeconds / visible) * contentWidth);
            const int x2 = contentLeft + static_cast<int>(((clip.startSeconds + clip.durationSeconds) / visible) * contentWidth);
            RECT clipRect{x1 + 1, trackTop + 5, std::max(x1 + 12, x2 - 1), trackBottom - 5};
            const bool selected = selectedClip_ && *selectedClip_ == i;
            Fill(dc, clipRect, selected ? kSelected : kClip);
            Frame(dc, clipRect, selected ? kAccent : kBorder);

            RECT nameRect = clipRect;
            nameRect.left += 8;
            nameRect.right -= 6;
            if (clip.mediaIndex < project_.Media().size()) {
                DrawLabel(dc, project_.Media()[clip.mediaIndex].name.c_str(), nameRect, kText);
            }
        }

        const int playheadX = contentLeft + static_cast<int>((project_.PlayheadSeconds() / visible) * contentWidth);
        HPEN playheadPen = CreatePen(PS_SOLID, 2, kAccent);
        HGDIOBJ oldPen = SelectObject(dc, playheadPen);
        MoveToEx(dc, playheadX, rulerTop, nullptr);
        LineTo(dc, playheadX, rect.bottom - 8);
        SelectObject(dc, oldPen);
        DeleteObject(playheadPen);
    }

    void PaintStatus(HDC dc, const RECT& rect) {
        Fill(dc, rect, RGB(18, 19, 21));
        RECT left = rect;
        left.left += 10;
        const std::wstring status = (playing_ ? L"재생 중    " : L"준비    ") + FormatTime(project_.PlayheadSeconds());
        DrawLabel(dc, status.c_str(), left, kMuted);

        RECT right = rect;
        right.right -= 10;
        DrawLabel(dc, L"Fillema 0.1 · Febius Creator Series", right, kMuted,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    std::optional<std::size_t> ClipAtPoint(int x, int y) const {
        const Layout layout = CalculateLayout();
        const int contentLeft = layout.timeline.left + 72;
        const int contentRight = layout.timeline.right - 12;
        const int contentWidth = std::max(1, contentRight - contentLeft);
        const int trackTop = layout.timeline.top + 66;
        const int trackBottom = std::min(layout.timeline.bottom - 14, trackTop + 72);
        if (y < trackTop || y > trackBottom || x < contentLeft || x > contentRight) return std::nullopt;

        const double visible = VisibleTimelineSeconds();
        for (std::size_t i = 0; i < project_.Clips().size(); ++i) {
            const Clip& clip = project_.Clips()[i];
            const int x1 = contentLeft + static_cast<int>((clip.startSeconds / visible) * contentWidth);
            const int x2 = contentLeft + static_cast<int>(((clip.startSeconds + clip.durationSeconds) / visible) * contentWidth);
            if (x >= x1 && x <= x2) return i;
        }
        return std::nullopt;
    }

    double TimelineSecondsAtX(int x) const {
        const Layout layout = CalculateLayout();
        const int contentLeft = layout.timeline.left + 72;
        const int contentRight = layout.timeline.right - 12;
        const int width = std::max(1, contentRight - contentLeft);
        const int clamped = std::clamp(x, contentLeft, contentRight);
        return (static_cast<double>(clamped - contentLeft) / static_cast<double>(width)) * VisibleTimelineSeconds();
    }

    void NewProject() {
        playing_ = false;
        KillTimer(hwnd_, kPlaybackTimer);
        project_.Clear();
        selectedClip_.reset();
        UpdateTitle();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OpenProject() {
        wchar_t path[MAX_PATH]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = L"Fillema 프로젝트 (*.fillema)\0*.fillema\0모든 파일 (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        dialog.lpstrDefExt = L"fillema";

        if (GetOpenFileNameW(&dialog)) {
            if (!project_.Load(path)) {
                ShowError(L"프로젝트를 열 수 없습니다. 파일이 손상되었거나 지원하지 않는 형식입니다.");
                return;
            }
            playing_ = false;
            KillTimer(hwnd_, kPlaybackTimer);
            selectedClip_.reset();
            UpdateTitle();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void SaveProject(bool saveAs) {
        std::wstring path = project_.FilePath();
        if (saveAs || path.empty()) {
            wchar_t buffer[MAX_PATH]{};
            if (!path.empty()) {
                wcsncpy_s(buffer, _countof(buffer), path.c_str(), _TRUNCATE);
            }

            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = hwnd_;
            dialog.lpstrFilter = L"Fillema 프로젝트 (*.fillema)\0*.fillema\0";
            dialog.lpstrFile = buffer;
            dialog.nMaxFile = MAX_PATH;
            dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            dialog.lpstrDefExt = L"fillema";
            if (!GetSaveFileNameW(&dialog)) return;
            path = buffer;
        }

        if (!project_.Save(path)) {
            ShowError(L"프로젝트를 저장할 수 없습니다.");
            return;
        }
        project_.SetFilePath(path);
        UpdateTitle();
    }

    void ImportMedia() {
        std::vector<wchar_t> buffer(32768, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = L"미디어 파일\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.mp3;*.wav;*.flac;*.m4a\0모든 파일 (*.*)\0*.*\0";
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&dialog)) return;

        const wchar_t* first = buffer.data();
        const wchar_t* second = first + wcslen(first) + 1;
        std::vector<std::wstring> paths;
        if (*second == L'\0') {
            paths.emplace_back(first);
        } else {
            const std::filesystem::path folder(first);
            for (const wchar_t* name = second; *name; name += wcslen(name) + 1) {
                paths.push_back((folder / name).wstring());
            }
        }

        double start = project_.EndSeconds();
        for (const auto& mediaPath : paths) {
            const std::size_t media = project_.AddMedia(mediaPath);
            selectedClip_ = project_.AddClip(media, start, 10.0);
            start += 10.0;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void TogglePlayback() {
        if (project_.Clips().empty()) return;
        if (!playing_ && project_.PlayheadSeconds() >= project_.EndSeconds()) {
            project_.SetPlayheadSeconds(0.0);
        }
        playing_ = !playing_;
        if (playing_) SetTimer(hwnd_, kPlaybackTimer, kPlaybackTimerMs, nullptr);
        else KillTimer(hwnd_, kPlaybackTimer);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SplitSelected() {
        if (!selectedClip_) return;
        const auto newIndex = project_.Split(*selectedClip_, project_.PlayheadSeconds());
        if (newIndex) {
            selectedClip_ = *newIndex;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void DeleteSelected() {
        if (!selectedClip_) return;
        if (project_.Remove(*selectedClip_)) {
            selectedClip_.reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void UpdateTitle() {
        std::wstring title = kAppName;
        if (!project_.FilePath().empty()) {
            title += L" — ";
            title += std::filesystem::path(project_.FilePath()).filename().wstring();
        } else {
            title += L" — 새 프로젝트";
        }
        SetWindowTextW(hwnd_, title.c_str());
    }

    void ShowError(const wchar_t* message) const {
        MessageBoxW(hwnd_, message, kAppName, MB_OK | MB_ICONERROR);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT fontBold_ = nullptr;
    Project project_;
    std::optional<std::size_t> selectedClip_;
    bool playing_ = false;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDPIAware();

    App app(instance);
    if (!app.Initialize(showCommand)) {
        MessageBoxW(nullptr, L"Fillema를 시작할 수 없습니다.", L"Febius Fillema", MB_OK | MB_ICONERROR);
        return 1;
    }
    return app.Run();
}
