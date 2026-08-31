#include "player.hpp"

#include <propvarutil.h>

namespace fillema {

STDMETHODIMP PlayerCallback::QueryInterface(REFIID iid, void** object) {
    if (!object) return E_POINTER;
    if (iid == IID_IUnknown || iid == __uuidof(IMFPMediaPlayerCallback)) {
        *object = static_cast<IMFPMediaPlayerCallback*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) PlayerCallback::AddRef() { return ++references_; }
STDMETHODIMP_(ULONG) PlayerCallback::Release() {
    const ULONG value = --references_;
    if (value == 0) delete this;
    return value;
}

void STDMETHODCALLTYPE PlayerCallback::OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) {
    if (owner_) owner_->onEvent(eventHeader);
}

PreviewPlayer::~PreviewPlayer() { shutdown(); }

bool PreviewPlayer::initialize(HWND videoWindow) {
    shutdown();
    videoWindow_ = videoWindow;
    callback_ = new PlayerCallback(this);
    const HRESULT result = MFPCreateMediaPlayer(nullptr, FALSE, 0, callback_, videoWindow_, &player_);
    if (FAILED(result)) {
        callback_->Release();
        callback_ = nullptr;
        videoWindow_ = nullptr;
        return false;
    }
    player_->SetAspectRatioMode(MFVideoARMode_PreservePicture);
    return true;
}

void PreviewPlayer::shutdown() {
    ready_ = false;
    if (player_) {
        player_->Shutdown();
        player_->Release();
        player_ = nullptr;
    }
    if (callback_) {
        callback_->Release();
        callback_ = nullptr;
    }
    videoWindow_ = nullptr;
}

bool PreviewPlayer::load(const std::filesystem::path& path, double start, double end, bool autoplay) {
    if (!player_ || path.empty()) return false;
    requestedStart_ = start;
    requestedEnd_ = end;
    autoplay_.store(autoplay);
    ready_.store(false);
    return SUCCEEDED(player_->CreateMediaItemFromURL(path.c_str(), FALSE, 0, nullptr));
}

void PreviewPlayer::play() {
    if (player_ && ready_.load()) player_->Play();
}
void PreviewPlayer::pause() {
    if (player_) player_->Pause();
}
void PreviewPlayer::toggle() {
    if (playing()) pause(); else play();
}

void PreviewPlayer::seek(double seconds) {
    if (!player_ || !ready_.load()) return;
    PROPVARIANT position;
    PropVariantInit(&position);
    if (SUCCEEDED(InitPropVariantFromInt64(static_cast<LONGLONG>(seconds * 10'000'000.0), &position))) {
        player_->SetPosition(MFP_POSITIONTYPE_100NS, &position);
    }
    PropVariantClear(&position);
}

double PreviewPlayer::position() const {
    if (!player_ || !ready_.load()) return requestedStart_;
    PROPVARIANT position;
    PropVariantInit(&position);
    double result = requestedStart_;
    if (SUCCEEDED(player_->GetPosition(MFP_POSITIONTYPE_100NS, &position)) && position.vt == VT_I8) {
        result = static_cast<double>(position.hVal.QuadPart) / 10'000'000.0;
    }
    PropVariantClear(&position);
    return result;
}

bool PreviewPlayer::playing() const {
    if (!player_) return false;
    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
    return SUCCEEDED(player_->GetState(&state)) && state == MFP_MEDIAPLAYER_STATE_PLAYING;
}

void PreviewPlayer::updateVideo() {
    if (player_) player_->UpdateVideo();
}

void PreviewPlayer::onEvent(MFP_EVENT_HEADER* eventHeader) {
    if (!eventHeader || FAILED(eventHeader->hrEvent) || !player_) return;
    switch (eventHeader->eEventType) {
    case MFP_EVENT_TYPE_MEDIAITEM_CREATED: {
        auto* event = MFP_GET_MEDIAITEM_CREATED_EVENT(eventHeader);
        if (event && event->pMediaItem) player_->SetMediaItem(event->pMediaItem);
        break;
    }
    case MFP_EVENT_TYPE_MEDIAITEM_SET:
        ready_.store(true);
        seek(requestedStart_);
        if (autoplay_.load()) player_->Play();
        break;
    case MFP_EVENT_TYPE_PLAYBACK_ENDED:
        ready_.store(true);
        break;
    default:
        break;
    }
}

} // namespace fillema
