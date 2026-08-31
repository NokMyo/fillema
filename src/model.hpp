#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fillema {

inline constexpr int kProjectFormatVersion = 1;

enum class PerformanceMode { Automatic, Quality, Balanced, Speed };
enum class LookPreset { None, CinemaNeutral, WarmFilm, ColdFilm, Bleach, SoftPortrait, Night, Vintage };
enum class VideoCodec { H264, H265 };
enum class ExportQuality { Compact, Standard, High };
enum class TextPosition { Top, Center, Bottom, Custom };

struct MediaItem {
    std::int64_t id = 0;
    std::filesystem::path path;
    std::string displayName;
    std::string codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration = 0.0;
    bool hasAudio = true;
    bool probeComplete = false;
    std::filesystem::path proxyPath;
    bool proxyReady = false;
};

struct ColorGrade {
    double exposure = 0.0;       // stops, -5..5
    double contrast = 0.0;       // -100..100
    double highlights = 0.0;     // -100..100
    double shadows = 0.0;        // -100..100
    double whites = 0.0;         // -100..100
    double blacks = 0.0;         // -100..100
    double temperature = 0.0;    // -100..100
    double tint = 0.0;           // -100..100
    double saturation = 100.0;   // 0..200

    // Three-point luma curve offsets and RGB tonal wheels.
    double curveShadows = 0.0;
    double curveMidtones = 0.0;
    double curveHighlights = 0.0;
    std::array<double, 3> shadowWheel{0.0, 0.0, 0.0};
    std::array<double, 3> midtoneWheel{0.0, 0.0, 0.0};
    std::array<double, 3> highlightWheel{0.0, 0.0, 0.0};

    LookPreset look = LookPreset::None;
    double lookStrength = 0.0;
    double grain = 0.0;
    double vignette = 0.0;
    double letterbox = 0.0;
};

struct AudioSettings {
    double volumeDb = 0.0;
    double fadeIn = 0.0;
    double fadeOut = 0.0;
    double eqLow = 0.0;
    double eqMid = 0.0;
    double eqHigh = 0.0;
    bool muted = false;
    bool normalize = false;
};

struct TextOverlay {
    std::int64_t id = 0;
    std::string text;
    std::string font = "Malgun Gothic";
    double fontSize = 48.0;
    TextPosition position = TextPosition::Bottom;
    double x = 0.5;
    double y = 0.86;
    double opacity = 1.0;
    double fadeIn = 0.25;
    double fadeOut = 0.25;
};

struct Clip {
    std::int64_t id = 0;
    std::int64_t mediaId = 0;
    double inPoint = 0.0;
    double outPoint = 0.0;
    double speed = 1.0;
    ColorGrade color;
    AudioSettings audio;
    std::vector<TextOverlay> texts;

    [[nodiscard]] double sourceDuration() const noexcept;
    [[nodiscard]] double timelineDuration() const noexcept;
};

struct OutputSettings {
    int width = 1920;
    int height = 1080;
    double fps = 0.0; // 0 follows the first source.
    VideoCodec codec = VideoCodec::H264;
    ExportQuality quality = ExportQuality::High;
    bool preserveAudio = true;
};

struct Project {
    int formatVersion = kProjectFormatVersion;
    std::string name = "제목 없음";
    std::vector<MediaItem> media;
    std::vector<Clip> timeline;
    OutputSettings output;
    PerformanceMode performanceMode = PerformanceMode::Automatic;
    std::filesystem::path filePath;
    bool dirty = false;
    std::int64_t nextId = 1;

    [[nodiscard]] std::int64_t allocateId() noexcept;
    [[nodiscard]] MediaItem* findMedia(std::int64_t id) noexcept;
    [[nodiscard]] const MediaItem* findMedia(std::int64_t id) const noexcept;
    [[nodiscard]] Clip* findClip(std::int64_t id) noexcept;
    [[nodiscard]] const Clip* findClip(std::int64_t id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> clipIndex(std::int64_t id) const noexcept;
    [[nodiscard]] double timelineDuration() const noexcept;
};

struct ValidationIssue {
    enum class Severity { Warning, Error } severity = Severity::Warning;
    std::string message;
};

[[nodiscard]] std::string ToString(PerformanceMode mode);
[[nodiscard]] std::string ToString(LookPreset preset);
[[nodiscard]] std::string ToString(VideoCodec codec);
[[nodiscard]] std::string ToString(ExportQuality quality);
[[nodiscard]] std::string ToString(TextPosition position);
[[nodiscard]] PerformanceMode PerformanceModeFromString(std::string_view value);
[[nodiscard]] LookPreset LookPresetFromString(std::string_view value);
[[nodiscard]] VideoCodec VideoCodecFromString(std::string_view value);
[[nodiscard]] ExportQuality ExportQualityFromString(std::string_view value);
[[nodiscard]] TextPosition TextPositionFromString(std::string_view value);

[[nodiscard]] std::vector<ValidationIssue> ValidateProject(Project& project);
[[nodiscard]] std::string SerializeProject(const Project& project);
[[nodiscard]] Project DeserializeProject(std::string_view contents);
void SaveProjectFile(const Project& project, const std::filesystem::path& path);
[[nodiscard]] Project LoadProjectFile(const std::filesystem::path& path);

[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view path);

} // namespace fillema

