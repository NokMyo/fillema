#pragma once

#if !defined(_WIN32)
#error app.hpp is only available on Windows.
#endif

#include "background_worker.hpp"
#include "ffmpeg.hpp"
#include "model.hpp"
#include "player.hpp"

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fillema {

class App final {
public:
    explicit App(HINSTANCE instance);
    ~App();

    bool initialize(int showCommand);
    int run();

    static LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TimelineWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK InspectorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StillWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SplashWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK CommandPanelProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK CommandEditSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);

private:
    struct TimelineHit {
        std::size_t index = 0;
        double globalTime = 0.0;
        double sourceTime = 0.0;
        bool valid = false;
    };

    bool registerWindowClasses();
    bool createMainWindow(int showCommand);
    void createMenu();
    void createControls();
    void createSplash();
    void layout(int width, int height);
    void paintMain(HDC context);
    void paintTimeline(HWND window, HDC context);
    void paintStill(HWND window, HDC context);
    void paintSplash(HWND window, HDC context);
    void drawMediaItem(const DRAWITEMSTRUCT& draw);
    void applyDarkMode(HWND window);
    void setFont(HWND window, bool semibold = false);

    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void handleCommand(int id, int notification, HWND source);
    void handleInspectorScroll(HWND control, int request);
    void handleDrop(HDROP drop);
    bool handleShortcut(WPARAM key);
    void showAbout();

    void newProject();
    void openProject();
    bool saveProject(bool saveAs = false);
    bool confirmDiscardChanges();
    void autosave();
    void refreshFromProject();
    void refreshMediaList();
    void updateWindowTitle();
    void setStatus(const std::wstring& text);
    void markDirty();
    void recordUndo();
    void undo();
    void redo();

    void importDialog();
    void importPaths(const std::vector<std::filesystem::path>& paths, bool addToTimeline);
    void probeMedia(std::int64_t mediaId, std::filesystem::path path);
    void maybeCreateProxy(std::int64_t mediaId, const MediaItem& media);
    void addMediaToTimeline(std::size_t mediaIndex);
    void selectClip(std::int64_t clipId, double sourceTime = -1.0, bool autoplay = false);
    void selectTimelineTime(double globalTime, bool autoplay = false);
    [[nodiscard]] double clipTimelineStart(std::size_t index) const;
    [[nodiscard]] TimelineHit hitTimeline(HWND timelineWindow, int x, int y) const;
    void splitSelected();
    void rippleDeleteSelected();
    void trimSelected(bool head);
    void duplicateSelected();
    void copySelected();
    void pasteClip();
    void reorderSelected(std::size_t targetIndex);
    void updatePlayhead();
    void togglePlayback();

    void buildInspector();
    void clearInspector();
    HWND addInspectorLabel(const std::wstring& text, int x, int y, int width, int height = 22, bool semibold = false);
    HWND addInspectorButton(const std::wstring& text, int id, int x, int y, int width, int height = 30);
    HWND addInspectorTrack(const std::wstring& label, int id, int minimum, int maximum, int value, int y,
        const std::wstring& displayValue);
    HWND addInspectorCombo(const std::wstring& label, int id, const std::vector<std::wstring>& items,
        int selected, int y);
    HWND addInspectorCheck(const std::wstring& label, int id, bool checked, int y);
    HWND addInspectorEdit(const std::wstring& label, int id, const std::wstring& value, int y,
        int height = 28, DWORD style = 0);
    void updateValueLabel(int propertyId, const std::wstring& value);
    [[nodiscard]] Clip* selectedClip();
    [[nodiscard]] const Clip* selectedClip() const;
    void switchInspectorTab(int tab);
    void scheduleAccuratePreview();
    void renderAccuratePreview();
    void clearPreviewBitmap();

    void exportDialog();
    void beginExport(const std::filesystem::path& outputPath);

    void showCommandBar();
    void hideCommandBar();
    void filterCommands();
    void executeCommandSelection();
    void setFullscreen(bool enabled);

    [[nodiscard]] std::filesystem::path recoveryPath() const;
    [[nodiscard]] std::wstring formatTime(double seconds) const;
    [[nodiscard]] std::wstring mediaDisplayLine(const MediaItem& media) const;

    HINSTANCE instance_ = nullptr;
    HWND mainWindow_ = nullptr;
    HWND splashWindow_ = nullptr;
    HWND mediaHeader_ = nullptr;
    HWND mediaList_ = nullptr;
    HWND previewWindow_ = nullptr;
    HWND stillWindow_ = nullptr;
    HWND timelineWindow_ = nullptr;
    HWND inspectorWindow_ = nullptr;
    HWND statusWindow_ = nullptr;
    HWND playButton_ = nullptr;
    HWND timeLabel_ = nullptr;
    HWND performanceCombo_ = nullptr;
    HWND commandPanel_ = nullptr;
    HWND commandEdit_ = nullptr;
    HWND commandList_ = nullptr;
    HMENU mainMenu_ = nullptr;
    std::vector<HWND> toolbarControls_;
    std::vector<HWND> inspectorControls_;
    std::unordered_map<int, HWND> valueLabels_;

    HFONT uiFont_ = nullptr;
    HFONT semiboldFont_ = nullptr;
    HBRUSH windowBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    HBITMAP previewBitmap_ = nullptr;

    Project project_;
    std::vector<Project> undoStack_;
    std::vector<Project> redoStack_;
    std::optional<Clip> copiedClip_;
    std::int64_t selectedClipId_ = 0;
    int selectedMediaIndex_ = -1;
    int inspectorTab_ = 0;
    int activeSliderId_ = -1;
    bool buildingInspector_ = false;
    bool exporting_ = false;
    bool showAccurateStill_ = false;
    bool fullscreen_ = false;
    bool draggingTimeline_ = false;
    std::int64_t draggingClipId_ = 0;
    std::uint64_t previewGeneration_ = 0;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR savedStyle_ = 0;
    double playheadTimeline_ = 0.0;

    PreviewPlayer player_;
    TaskQueue mediaWorker_;
    TaskQueue exportWorker_;
    LatestTaskWorker previewWorker_;
    std::filesystem::path ffmpegPath_;
    std::filesystem::path ffprobePath_;
    std::filesystem::path cacheDirectory_;
    AdaptiveProfile adaptiveProfile_;
    unsigned logicalProcessors_ = 1;
    std::uint64_t physicalMemoryMb_ = 0;

    std::vector<std::pair<std::wstring, int>> commands_;
};

} // namespace fillema
