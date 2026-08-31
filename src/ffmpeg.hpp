#pragma once

#include "model.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fillema {

struct AdaptiveProfile {
    int previewWidth = 1280;
    int previewHeight = 720;
    int workerThreads = 2;
    bool createProxy = false;
    bool deferHeavyEffects = false;
    std::string label;
    std::string reason;
};

struct ProbeInfo {
    std::string codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration = 0.0;
    bool hasAudio = false;
    bool valid = false;
};

struct ExportPlan {
    std::vector<std::string> arguments;
    std::string filterComplex;
    std::vector<std::string> warnings;
};

[[nodiscard]] AdaptiveProfile ChooseAdaptiveProfile(
    PerformanceMode mode,
    unsigned logicalProcessors,
    std::uint64_t physicalMemoryMb,
    const MediaItem* media = nullptr);

[[nodiscard]] ProbeInfo ParseProbeOutput(std::string_view output);
[[nodiscard]] std::vector<std::string> BuildProbeArguments(const std::filesystem::path& input);
[[nodiscard]] std::vector<std::string> BuildProxyArguments(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const AdaptiveProfile& profile);
[[nodiscard]] std::vector<std::string> BuildPreviewStillArguments(
    const MediaItem& media,
    const Clip& clip,
    double sourcePosition,
    const std::filesystem::path& outputBmp,
    int width,
    int height,
    bool deferHeavyEffects);
[[nodiscard]] ExportPlan BuildExportPlan(
    const Project& project,
    const std::filesystem::path& filterScriptPath,
    const std::filesystem::path& outputPath);

[[nodiscard]] std::string BuildColorFilter(const ColorGrade& color, bool deferHeavyEffects = false);
[[nodiscard]] std::string BuildAtempoFilter(double speed);
[[nodiscard]] std::string EscapeDrawText(std::string_view text);

} // namespace fillema

