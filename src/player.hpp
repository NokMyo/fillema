#pragma once

#if !defined(_WIN32)
#error player.hpp is only available on Windows.
#endif

#include <windows.h>
#include <mfplay.h>

#include <atomic>
#include <filesystem>

namespace fillema {

class PreviewPlayer;

class PlayerCallback final : public IMFPMediaPlayerCallback {
public:
    explicit PlayerCallback(PreviewPlayer* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) override;

private:
    std::atomic<ULONG> references_{1};
    PreviewPlayer* owner_ = nullptr;
};

class PreviewPlayer final {
public:
    PreviewPlayer() = default;
    ~PreviewPlayer();
    PreviewPlayer(const PreviewPlayer&) = delete;
    PreviewPlayer& operator=(const PreviewPlayer&) = delete;

    bool initialize(HWND videoWindow);
    void shutdown();
    bool load(const std::filesystem::path& path, double start, double end, bool autoplay = false);
    void play();
    void pause();
    void toggle();
    void seek(double seconds);
    [[nodiscard]] double position() const;
    [[nodiscard]] bool playing() const;
    [[nodiscard]] bool ready() const noexcept { return ready_.load(); }
    void updateVideo();
    void onEvent(MFP_EVENT_HEADER* eventHeader);

private:
    friend class PlayerCallback;
    IMFPMediaPlayer* player_ = nullptr;
    PlayerCallback* callback_ = nullptr;
    HWND videoWindow_ = nullptr;
    double requestedStart_ = 0.0;
    double requestedEnd_ = 0.0;
    std::atomic_bool autoplay_{false};
    std::atomic_bool ready_{false};
};

} // namespace fillema
