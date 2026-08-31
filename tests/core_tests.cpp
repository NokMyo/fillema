#include "ffmpeg.hpp"
#include "json.hpp"
#include "model.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool Near(double left, double right, double epsilon = 0.0001) {
    return std::abs(left - right) <= epsilon;
}

fillema::Project SampleProject() {
    fillema::Project project;
    project.name = "한강 야간 촬영";
    project.performanceMode = fillema::PerformanceMode::Automatic;

    fillema::MediaItem media;
    media.id = project.allocateId();
    media.path = fillema::PathFromUtf8("C:/영상/원본 01.mp4");
    media.displayName = "원본 01.mp4";
    media.codec = "hevc";
    media.width = 3840;
    media.height = 2160;
    media.fps = 29.97;
    media.duration = 12.5;
    media.hasAudio = true;
    media.probeComplete = true;
    project.media.push_back(media);

    fillema::Clip clip;
    clip.id = project.allocateId();
    clip.mediaId = media.id;
    clip.inPoint = 1.25;
    clip.outPoint = 10.25;
    clip.speed = 0.5;
    clip.color.exposure = 0.75;
    clip.color.temperature = 18.0;
    clip.color.saturation = 112.0;
    clip.color.look = fillema::LookPreset::WarmFilm;
    clip.color.lookStrength = 65.0;
    clip.audio.volumeDb = -2.5;
    clip.audio.fadeIn = 0.5;
    clip.audio.normalize = true;
    fillema::TextOverlay text;
    text.id = project.allocateId();
    text.text = "SEOUL : NIGHT";
    clip.texts.push_back(text);
    project.timeline.push_back(clip);
    return project;
}

void TestJson() {
    const auto value = fillema::json::Parse(R"({"name":"Fillema","active":true,"items":[1,2,3],"unicode":"\uD55C\uAE00"})");
    Require(value.isObject(), "JSON root should be an object");
    Require(value.find("active") && value.find("active")->asBool(), "JSON boolean lost");
    Require(value.find("items") && value.find("items")->asArray().size() == 3, "JSON array lost");
    const std::string roundTrip = fillema::json::Stringify(value);
    Require(fillema::json::Parse(roundTrip).find("unicode") != nullptr, "JSON round trip failed");
}

void TestProjectRoundTrip() {
    const fillema::Project original = SampleProject();
    fillema::Project restored = fillema::DeserializeProject(fillema::SerializeProject(original));
    Require(restored.name == original.name, "project name changed");
    Require(restored.media.size() == 1 && restored.timeline.size() == 1, "project item count changed");
    Require(restored.media.front().path == original.media.front().path, "unicode media path changed");
    Require(Near(restored.timeline.front().timelineDuration(), 18.0), "clip speed duration is wrong");
    Require(restored.timeline.front().color.look == fillema::LookPreset::WarmFilm, "look preset changed");
    Require(restored.timeline.front().texts.front().text == "SEOUL : NIGHT", "text overlay changed");
}

void TestLegacyProjectImport() {
    const std::string legacy =
        "FILLEMA 1\n"
        "PLAYHEAD 2.5\n"
        "MEDIA \"C:/video/one.mp4\"\n"
        "CLIP 0 0 4.25\n"
        "CLIP 0 4.25 2.5\n";
    const auto project = fillema::DeserializeProject(legacy);
    Require(project.media.size() == 1 && project.timeline.size() == 2, "legacy project items were not imported");
    Require(Near(project.timelineDuration(), 6.75), "legacy clip duration changed");
    Require(project.dirty, "legacy import should be saved in the new format");
}

void TestAdaptiveEditing() {
    const auto project = SampleProject();
    const auto low = fillema::ChooseAdaptiveProfile(fillema::PerformanceMode::Automatic, 2, 4096, &project.media.front());
    Require(low.previewWidth == 640 && low.createProxy && low.deferHeavyEffects, "low-end auto profile is too heavy");
    const auto quality = fillema::ChooseAdaptiveProfile(fillema::PerformanceMode::Quality, 2, 4096, &project.media.front());
    Require(quality.previewWidth == 1920 && !quality.deferHeavyEffects, "quality mode should honor user choice");
}

void TestProbeParsing() {
    const auto probe = fillema::ParseProbeOutput(
        "codec_name=hevc\nwidth=3840\nheight=2160\navg_frame_rate=30000/1001\nduration=12.500000\ncodec_type=audio\n");
    Require(probe.valid, "probe data should be valid");
    Require(probe.codec == "hevc" && probe.width == 3840 && probe.hasAudio, "probe fields are wrong");
    Require(Near(probe.fps, 29.97002997, 0.001), "frame rate fraction parsing failed");
}

void TestFiltersAndExport() {
    const auto project = SampleProject();
    const std::string color = fillema::BuildColorFilter(project.timeline.front().color);
    Require(color.find("exposure=") != std::string::npos, "exposure filter missing");
    Require(color.find("colorbalance=") != std::string::npos, "temperature/tint filter missing");
    Require(color.find("saturation=") != std::string::npos, "saturation filter missing");
    Require(fillema::BuildAtempoFilter(8.0) == "atempo=2,atempo=2,atempo=2", "8x atempo chain is invalid");
    Require(fillema::BuildAtempoFilter(0.25) == "atempo=0.5,atempo=0.5", "0.25x atempo chain is invalid");

    const auto plan = fillema::BuildExportPlan(project, "filters.txt", "out.mp4");
    Require(plan.warnings.empty(), "valid project should export without warnings");
    Require(plan.filterComplex.find("concat=n=1:v=1:a=1") != std::string::npos, "concat graph missing");
    Require(plan.filterComplex.find("drawtext=") != std::string::npos, "text overlay graph missing");
    Require(plan.filterComplex.find("loudnorm=") != std::string::npos, "normalization graph missing");
    Require(!plan.arguments.empty() && plan.arguments.back() == "out.mp4", "output path missing");

    auto silentProject = project;
    silentProject.output.preserveAudio = false;
    const auto silentPlan = fillema::BuildExportPlan(silentProject, "filters.txt", "silent.mp4");
    Require(silentPlan.filterComplex.find("concat=n=1:v=1:a=0") != std::string::npos, "silent concat graph is invalid");
    Require(silentPlan.filterComplex.find("[aout]") == std::string::npos, "silent export leaves an audio output connected");
}

} // namespace

int main() {
    try {
        TestJson();
        TestProjectRoundTrip();
        TestLegacyProjectImport();
        TestAdaptiveEditing();
        TestProbeParsing();
        TestFiltersAndExport();
        std::cout << "Fillema core tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Fillema core test failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
