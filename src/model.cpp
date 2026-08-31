#include "model.hpp"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace fillema {
namespace {

using json::Value;

double Clamp(double value, double minimum, double maximum) {
    return std::clamp(std::isfinite(value) ? value : minimum, minimum, maximum);
}

const Value* Find(const Value& value, std::string_view key) {
    return value.find(key);
}

double Number(const Value& value, std::string_view key, double fallback = 0.0) {
    if (const auto* item = Find(value, key)) return item->asNumber(fallback);
    return fallback;
}

long long Integer(const Value& value, std::string_view key, long long fallback = 0) {
    if (const auto* item = Find(value, key)) return item->asInteger(fallback);
    return fallback;
}

bool Boolean(const Value& value, std::string_view key, bool fallback = false) {
    if (const auto* item = Find(value, key)) return item->asBool(fallback);
    return fallback;
}

std::string String(const Value& value, std::string_view key, std::string fallback = {}) {
    if (const auto* item = Find(value, key); item && item->isString()) return item->asString();
    return fallback;
}

Value Array3(const std::array<double, 3>& value) {
    return Value::Array{Value(value[0]), Value(value[1]), Value(value[2])};
}

std::array<double, 3> ReadArray3(const Value* value) {
    std::array<double, 3> result{};
    if (!value || !value->isArray()) return result;
    const auto& array = value->asArray();
    for (std::size_t i = 0; i < result.size() && i < array.size(); ++i) result[i] = array[i].asNumber();
    return result;
}

Value ColorToJson(const ColorGrade& color) {
    Value value{Value::Object{}};
    value["exposure"] = color.exposure;
    value["contrast"] = color.contrast;
    value["highlights"] = color.highlights;
    value["shadows"] = color.shadows;
    value["whites"] = color.whites;
    value["blacks"] = color.blacks;
    value["temperature"] = color.temperature;
    value["tint"] = color.tint;
    value["saturation"] = color.saturation;
    value["curve_shadows"] = color.curveShadows;
    value["curve_midtones"] = color.curveMidtones;
    value["curve_highlights"] = color.curveHighlights;
    value["shadow_wheel"] = Array3(color.shadowWheel);
    value["midtone_wheel"] = Array3(color.midtoneWheel);
    value["highlight_wheel"] = Array3(color.highlightWheel);
    value["look"] = ToString(color.look);
    value["look_strength"] = color.lookStrength;
    value["grain"] = color.grain;
    value["vignette"] = color.vignette;
    value["letterbox"] = color.letterbox;
    return value;
}

ColorGrade ColorFromJson(const Value* value) {
    ColorGrade color;
    if (!value || !value->isObject()) return color;
    color.exposure = Number(*value, "exposure");
    color.contrast = Number(*value, "contrast");
    color.highlights = Number(*value, "highlights");
    color.shadows = Number(*value, "shadows");
    color.whites = Number(*value, "whites");
    color.blacks = Number(*value, "blacks");
    color.temperature = Number(*value, "temperature");
    color.tint = Number(*value, "tint");
    color.saturation = Number(*value, "saturation", 100.0);
    color.curveShadows = Number(*value, "curve_shadows");
    color.curveMidtones = Number(*value, "curve_midtones");
    color.curveHighlights = Number(*value, "curve_highlights");
    color.shadowWheel = ReadArray3(Find(*value, "shadow_wheel"));
    color.midtoneWheel = ReadArray3(Find(*value, "midtone_wheel"));
    color.highlightWheel = ReadArray3(Find(*value, "highlight_wheel"));
    color.look = LookPresetFromString(String(*value, "look"));
    color.lookStrength = Number(*value, "look_strength");
    color.grain = Number(*value, "grain");
    color.vignette = Number(*value, "vignette");
    color.letterbox = Number(*value, "letterbox");
    return color;
}

Value AudioToJson(const AudioSettings& audio) {
    Value value{Value::Object{}};
    value["volume_db"] = audio.volumeDb;
    value["fade_in"] = audio.fadeIn;
    value["fade_out"] = audio.fadeOut;
    value["eq_low"] = audio.eqLow;
    value["eq_mid"] = audio.eqMid;
    value["eq_high"] = audio.eqHigh;
    value["muted"] = audio.muted;
    value["normalize"] = audio.normalize;
    return value;
}

AudioSettings AudioFromJson(const Value* value) {
    AudioSettings audio;
    if (!value || !value->isObject()) return audio;
    audio.volumeDb = Number(*value, "volume_db");
    audio.fadeIn = Number(*value, "fade_in");
    audio.fadeOut = Number(*value, "fade_out");
    audio.eqLow = Number(*value, "eq_low");
    audio.eqMid = Number(*value, "eq_mid");
    audio.eqHigh = Number(*value, "eq_high");
    audio.muted = Boolean(*value, "muted");
    audio.normalize = Boolean(*value, "normalize");
    return audio;
}

Value TextToJson(const TextOverlay& text) {
    Value value{Value::Object{}};
    value["id"] = static_cast<long long>(text.id);
    value["text"] = text.text;
    value["font"] = text.font;
    value["font_size"] = text.fontSize;
    value["position"] = ToString(text.position);
    value["x"] = text.x;
    value["y"] = text.y;
    value["opacity"] = text.opacity;
    value["fade_in"] = text.fadeIn;
    value["fade_out"] = text.fadeOut;
    return value;
}

TextOverlay TextFromJson(const Value& value) {
    TextOverlay text;
    text.id = Integer(value, "id");
    text.text = String(value, "text");
    text.font = String(value, "font", "Malgun Gothic");
    text.fontSize = Number(value, "font_size", 48.0);
    text.position = TextPositionFromString(String(value, "position", "bottom"));
    text.x = Number(value, "x", 0.5);
    text.y = Number(value, "y", 0.86);
    text.opacity = Number(value, "opacity", 1.0);
    text.fadeIn = Number(value, "fade_in", 0.25);
    text.fadeOut = Number(value, "fade_out", 0.25);
    return text;
}

Project DeserializeLegacyProject(std::string_view contents) {
    std::istringstream stream{std::string(contents)};
    std::string signature;
    int version = 0;
    if (!(stream >> signature >> version) || signature != "FILLEMA" || version != 1) {
        throw std::runtime_error("Fillema 프로젝트 파일이 아닙니다.");
    }
    Project project;
    std::vector<std::tuple<std::size_t, double, double>> legacyClips;
    std::string token;
    while (stream >> token) {
        if (token == "PLAYHEAD") {
            double ignored = 0.0;
            stream >> ignored;
        } else if (token == "MEDIA") {
            std::string path;
            stream >> std::quoted(path);
            if (!stream) throw std::runtime_error("기존 프로젝트의 미디어 경로가 손상되었습니다.");
            MediaItem media;
            media.id = project.allocateId();
            media.path = PathFromUtf8(path);
            media.displayName = PathToUtf8(media.path.filename());
            media.duration = 5.0;
            project.media.push_back(std::move(media));
        } else if (token == "CLIP") {
            std::size_t mediaIndex = 0;
            double timelineStart = 0.0;
            double duration = 0.0;
            stream >> mediaIndex >> timelineStart >> duration;
            if (!stream) throw std::runtime_error("기존 프로젝트의 클립 정보가 손상되었습니다.");
            legacyClips.emplace_back(mediaIndex, timelineStart, duration);
        } else {
            std::string ignored;
            std::getline(stream, ignored);
        }
    }
    std::stable_sort(legacyClips.begin(), legacyClips.end(), [](const auto& left, const auto& right) {
        return std::get<1>(left) < std::get<1>(right);
    });
    for (const auto& [mediaIndex, timelineStart, duration] : legacyClips) {
        (void)timelineStart;
        if (mediaIndex >= project.media.size() || duration <= 0.0) continue;
        Clip clip;
        clip.id = project.allocateId();
        clip.mediaId = project.media[mediaIndex].id;
        clip.inPoint = 0.0;
        clip.outPoint = duration;
        project.media[mediaIndex].duration = std::max(project.media[mediaIndex].duration, duration);
        project.timeline.push_back(std::move(clip));
    }
    project.name = "가져온 기존 프로젝트";
    project.dirty = true;
    (void)ValidateProject(project);
    return project;
}

} // namespace

double Clip::sourceDuration() const noexcept { return std::max(0.0, outPoint - inPoint); }
double Clip::timelineDuration() const noexcept { return sourceDuration() / std::max(0.01, speed); }

std::int64_t Project::allocateId() noexcept { return nextId++; }
MediaItem* Project::findMedia(std::int64_t id) noexcept {
    const auto iterator = std::find_if(media.begin(), media.end(), [id](const auto& item) { return item.id == id; });
    return iterator == media.end() ? nullptr : &*iterator;
}
const MediaItem* Project::findMedia(std::int64_t id) const noexcept {
    const auto iterator = std::find_if(media.begin(), media.end(), [id](const auto& item) { return item.id == id; });
    return iterator == media.end() ? nullptr : &*iterator;
}
Clip* Project::findClip(std::int64_t id) noexcept {
    const auto iterator = std::find_if(timeline.begin(), timeline.end(), [id](const auto& item) { return item.id == id; });
    return iterator == timeline.end() ? nullptr : &*iterator;
}
const Clip* Project::findClip(std::int64_t id) const noexcept {
    const auto iterator = std::find_if(timeline.begin(), timeline.end(), [id](const auto& item) { return item.id == id; });
    return iterator == timeline.end() ? nullptr : &*iterator;
}
std::optional<std::size_t> Project::clipIndex(std::int64_t id) const noexcept {
    const auto iterator = std::find_if(timeline.begin(), timeline.end(), [id](const auto& item) { return item.id == id; });
    if (iterator == timeline.end()) return std::nullopt;
    return static_cast<std::size_t>(std::distance(timeline.begin(), iterator));
}
double Project::timelineDuration() const noexcept {
    double duration = 0.0;
    for (const auto& clip : timeline) duration += clip.timelineDuration();
    return duration;
}

std::string ToString(PerformanceMode mode) {
    switch (mode) {
    case PerformanceMode::Quality: return "quality";
    case PerformanceMode::Balanced: return "balanced";
    case PerformanceMode::Speed: return "speed";
    default: return "automatic";
    }
}
std::string ToString(LookPreset preset) {
    switch (preset) {
    case LookPreset::CinemaNeutral: return "cinema_neutral";
    case LookPreset::WarmFilm: return "warm_film";
    case LookPreset::ColdFilm: return "cold_film";
    case LookPreset::Bleach: return "bleach";
    case LookPreset::SoftPortrait: return "soft_portrait";
    case LookPreset::Night: return "night";
    case LookPreset::Vintage: return "vintage";
    default: return "none";
    }
}
std::string ToString(VideoCodec codec) { return codec == VideoCodec::H265 ? "h265" : "h264"; }
std::string ToString(ExportQuality quality) {
    switch (quality) {
    case ExportQuality::Compact: return "compact";
    case ExportQuality::Standard: return "standard";
    default: return "high";
    }
}
std::string ToString(TextPosition position) {
    switch (position) {
    case TextPosition::Top: return "top";
    case TextPosition::Center: return "center";
    case TextPosition::Custom: return "custom";
    default: return "bottom";
    }
}

PerformanceMode PerformanceModeFromString(std::string_view value) {
    if (value == "quality") return PerformanceMode::Quality;
    if (value == "balanced") return PerformanceMode::Balanced;
    if (value == "speed") return PerformanceMode::Speed;
    return PerformanceMode::Automatic;
}
LookPreset LookPresetFromString(std::string_view value) {
    if (value == "cinema_neutral") return LookPreset::CinemaNeutral;
    if (value == "warm_film") return LookPreset::WarmFilm;
    if (value == "cold_film") return LookPreset::ColdFilm;
    if (value == "bleach") return LookPreset::Bleach;
    if (value == "soft_portrait") return LookPreset::SoftPortrait;
    if (value == "night") return LookPreset::Night;
    if (value == "vintage") return LookPreset::Vintage;
    return LookPreset::None;
}
VideoCodec VideoCodecFromString(std::string_view value) { return value == "h265" ? VideoCodec::H265 : VideoCodec::H264; }
ExportQuality ExportQualityFromString(std::string_view value) {
    if (value == "compact") return ExportQuality::Compact;
    if (value == "standard") return ExportQuality::Standard;
    return ExportQuality::High;
}
TextPosition TextPositionFromString(std::string_view value) {
    if (value == "top") return TextPosition::Top;
    if (value == "center") return TextPosition::Center;
    if (value == "custom") return TextPosition::Custom;
    return TextPosition::Bottom;
}

std::vector<ValidationIssue> ValidateProject(Project& project) {
    std::vector<ValidationIssue> issues;
    project.formatVersion = kProjectFormatVersion;
    project.output.width = std::clamp(project.output.width, 320, 7680);
    project.output.height = std::clamp(project.output.height, 240, 4320);
    project.output.width -= project.output.width % 2;
    project.output.height -= project.output.height % 2;
    project.output.fps = Clamp(project.output.fps, 0.0, 240.0);

    std::int64_t maximumId = 0;
    for (auto& media : project.media) {
        maximumId = std::max(maximumId, media.id);
        media.duration = std::max(0.0, media.duration);
        media.width = std::max(0, media.width);
        media.height = std::max(0, media.height);
        media.fps = Clamp(media.fps, 0.0, 240.0);
        if (media.path.empty()) issues.push_back({ValidationIssue::Severity::Warning, "경로가 비어 있는 미디어가 있습니다."});
    }
    for (auto& clip : project.timeline) {
        maximumId = std::max(maximumId, clip.id);
        const MediaItem* media = project.findMedia(clip.mediaId);
        if (!media) {
            issues.push_back({ValidationIssue::Severity::Error, "원본 미디어를 찾을 수 없는 클립이 있습니다."});
            continue;
        }
        const double maximumOut = media->duration > 0.0
            ? std::max(0.01, media->duration)
            : std::max({clip.outPoint, clip.inPoint + 0.01, 0.1});
        clip.inPoint = Clamp(clip.inPoint, 0.0, std::max(0.0, maximumOut - 0.01));
        clip.outPoint = Clamp(clip.outPoint, clip.inPoint + 0.01, maximumOut);
        clip.speed = Clamp(clip.speed, 0.1, 8.0);
        auto& color = clip.color;
        color.exposure = Clamp(color.exposure, -5.0, 5.0);
        color.contrast = Clamp(color.contrast, -100.0, 100.0);
        color.highlights = Clamp(color.highlights, -100.0, 100.0);
        color.shadows = Clamp(color.shadows, -100.0, 100.0);
        color.whites = Clamp(color.whites, -100.0, 100.0);
        color.blacks = Clamp(color.blacks, -100.0, 100.0);
        color.temperature = Clamp(color.temperature, -100.0, 100.0);
        color.tint = Clamp(color.tint, -100.0, 100.0);
        color.saturation = Clamp(color.saturation, 0.0, 200.0);
        color.lookStrength = Clamp(color.lookStrength, 0.0, 100.0);
        color.grain = Clamp(color.grain, 0.0, 100.0);
        color.vignette = Clamp(color.vignette, 0.0, 100.0);
        color.letterbox = Clamp(color.letterbox, 0.0, 20.0);
        for (auto* wheel : {&color.shadowWheel, &color.midtoneWheel, &color.highlightWheel}) {
            for (auto& channel : *wheel) channel = Clamp(channel, -100.0, 100.0);
        }
        auto& audio = clip.audio;
        audio.volumeDb = Clamp(audio.volumeDb, -60.0, 24.0);
        audio.fadeIn = Clamp(audio.fadeIn, 0.0, clip.timelineDuration());
        audio.fadeOut = Clamp(audio.fadeOut, 0.0, clip.timelineDuration());
        audio.eqLow = Clamp(audio.eqLow, -18.0, 18.0);
        audio.eqMid = Clamp(audio.eqMid, -18.0, 18.0);
        audio.eqHigh = Clamp(audio.eqHigh, -18.0, 18.0);
        for (auto& text : clip.texts) {
            maximumId = std::max(maximumId, text.id);
            text.fontSize = Clamp(text.fontSize, 8.0, 320.0);
            text.x = Clamp(text.x, 0.0, 1.0);
            text.y = Clamp(text.y, 0.0, 1.0);
            text.opacity = Clamp(text.opacity, 0.0, 1.0);
            text.fadeIn = Clamp(text.fadeIn, 0.0, clip.timelineDuration());
            text.fadeOut = Clamp(text.fadeOut, 0.0, clip.timelineDuration());
        }
    }
    project.nextId = std::max(project.nextId, maximumId + 1);
    return issues;
}

std::string SerializeProject(const Project& project) {
    Value root{Value::Object{}};
    root["format"] = "Febius Fillema Project";
    root["format_version"] = project.formatVersion;
    root["application_version"] = "0.1.0";
    root["name"] = project.name;
    root["performance_mode"] = ToString(project.performanceMode);
    root["next_id"] = static_cast<long long>(project.nextId);

    Value output{Value::Object{}};
    output["width"] = project.output.width;
    output["height"] = project.output.height;
    output["fps"] = project.output.fps;
    output["codec"] = ToString(project.output.codec);
    output["quality"] = ToString(project.output.quality);
    output["preserve_audio"] = project.output.preserveAudio;
    root["output"] = std::move(output);

    Value::Array mediaArray;
    mediaArray.reserve(project.media.size());
    for (const auto& media : project.media) {
        Value value{Value::Object{}};
        value["id"] = static_cast<long long>(media.id);
        value["path"] = PathToUtf8(media.path);
        value["display_name"] = media.displayName;
        value["codec"] = media.codec;
        value["width"] = media.width;
        value["height"] = media.height;
        value["fps"] = media.fps;
        value["duration"] = media.duration;
        value["has_audio"] = media.hasAudio;
        value["probe_complete"] = media.probeComplete;
        value["proxy_path"] = PathToUtf8(media.proxyPath);
        value["proxy_ready"] = media.proxyReady;
        mediaArray.push_back(std::move(value));
    }
    root["media"] = std::move(mediaArray);

    Value::Array timelineArray;
    timelineArray.reserve(project.timeline.size());
    for (const auto& clip : project.timeline) {
        Value value{Value::Object{}};
        value["id"] = static_cast<long long>(clip.id);
        value["media_id"] = static_cast<long long>(clip.mediaId);
        value["in"] = clip.inPoint;
        value["out"] = clip.outPoint;
        value["speed"] = clip.speed;
        value["color"] = ColorToJson(clip.color);
        value["audio"] = AudioToJson(clip.audio);
        Value::Array texts;
        for (const auto& text : clip.texts) texts.push_back(TextToJson(text));
        value["texts"] = std::move(texts);
        timelineArray.push_back(std::move(value));
    }
    root["timeline"] = std::move(timelineArray);
    return json::Stringify(root, true, 2);
}

Project DeserializeProject(std::string_view contents) {
    if (contents.starts_with("FILLEMA 1")) return DeserializeLegacyProject(contents);
    const Value root = json::Parse(contents);
    if (!root.isObject() || String(root, "format") != "Febius Fillema Project") {
        throw std::runtime_error("Fillema 프로젝트 파일이 아닙니다.");
    }
    const int version = static_cast<int>(Integer(root, "format_version", 0));
    if (version < 1 || version > kProjectFormatVersion) {
        throw std::runtime_error("지원하지 않는 Fillema 프로젝트 버전입니다.");
    }
    Project project;
    project.formatVersion = version;
    project.name = String(root, "name", "제목 없음");
    project.performanceMode = PerformanceModeFromString(String(root, "performance_mode"));
    project.nextId = Integer(root, "next_id", 1);

    if (const auto* output = Find(root, "output"); output && output->isObject()) {
        project.output.width = static_cast<int>(Integer(*output, "width", 1920));
        project.output.height = static_cast<int>(Integer(*output, "height", 1080));
        project.output.fps = Number(*output, "fps");
        project.output.codec = VideoCodecFromString(String(*output, "codec", "h264"));
        project.output.quality = ExportQualityFromString(String(*output, "quality", "high"));
        project.output.preserveAudio = Boolean(*output, "preserve_audio", true);
    }
    if (const auto* media = Find(root, "media"); media && media->isArray()) {
        for (const auto& value : media->asArray()) {
            if (!value.isObject()) continue;
            MediaItem item;
            item.id = Integer(value, "id");
            item.path = PathFromUtf8(String(value, "path"));
            item.displayName = String(value, "display_name");
            item.codec = String(value, "codec");
            item.width = static_cast<int>(Integer(value, "width"));
            item.height = static_cast<int>(Integer(value, "height"));
            item.fps = Number(value, "fps");
            item.duration = Number(value, "duration");
            item.hasAudio = Boolean(value, "has_audio", true);
            item.probeComplete = Boolean(value, "probe_complete");
            item.proxyPath = PathFromUtf8(String(value, "proxy_path"));
            item.proxyReady = Boolean(value, "proxy_ready");
            project.media.push_back(std::move(item));
        }
    }
    if (const auto* timeline = Find(root, "timeline"); timeline && timeline->isArray()) {
        for (const auto& value : timeline->asArray()) {
            if (!value.isObject()) continue;
            Clip clip;
            clip.id = Integer(value, "id");
            clip.mediaId = Integer(value, "media_id");
            clip.inPoint = Number(value, "in");
            clip.outPoint = Number(value, "out");
            clip.speed = Number(value, "speed", 1.0);
            clip.color = ColorFromJson(Find(value, "color"));
            clip.audio = AudioFromJson(Find(value, "audio"));
            if (const auto* texts = Find(value, "texts"); texts && texts->isArray()) {
                for (const auto& text : texts->asArray()) if (text.isObject()) clip.texts.push_back(TextFromJson(text));
            }
            project.timeline.push_back(std::move(clip));
        }
    }
    (void)ValidateProject(project);
    project.dirty = false;
    return project;
}

void SaveProjectFile(const Project& project, const std::filesystem::path& path) {
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("프로젝트 임시 파일을 만들 수 없습니다.");
        const std::string contents = SerializeProject(project);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) throw std::runtime_error("프로젝트 파일을 저장하지 못했습니다.");
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("프로젝트 파일을 교체하지 못했습니다.");
    }
}

Project LoadProjectFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("프로젝트 파일을 열 수 없습니다.");
    const std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    Project project = DeserializeProject(contents);
    project.filePath = path;
    return project;
}

std::string PathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
    return path.string();
#endif
}

std::filesystem::path PathFromUtf8(std::string_view path) {
#if defined(_WIN32)
    std::u8string value;
    value.resize(path.size());
    std::transform(path.begin(), path.end(), value.begin(), [](char c) { return static_cast<char8_t>(c); });
    return std::filesystem::path(value);
#else
    return std::filesystem::path(std::string(path));
#endif
}

} // namespace fillema
