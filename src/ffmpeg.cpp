#include "ffmpeg.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>

namespace fillema {
namespace {

double Clamp(double value, double minimum, double maximum) {
    return std::clamp(std::isfinite(value) ? value : 0.0, minimum, maximum);
}

std::string Decimal(double value, int precision = 5) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    std::string result = stream.str();
    while (result.size() > 1 && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    if (result == "-0") result = "0";
    return result;
}

double ParseFraction(std::string_view value) {
    const std::size_t slash = value.find('/');
    try {
        if (slash == std::string_view::npos) return std::stod(std::string(value));
        const double numerator = std::stod(std::string(value.substr(0, slash)));
        const double denominator = std::stod(std::string(value.substr(slash + 1)));
        return denominator == 0.0 ? 0.0 : numerator / denominator;
    } catch (...) {
        return 0.0;
    }
}

struct LookAdjustment {
    double exposure = 0.0;
    double contrast = 0.0;
    double saturation = 0.0;
    double temperature = 0.0;
    double tint = 0.0;
    double shadows = 0.0;
    double highlights = 0.0;
};

LookAdjustment LookFor(LookPreset preset) {
    switch (preset) {
    case LookPreset::CinemaNeutral: return {-0.05, 8.0, -3.0, 2.0, 0.0, 4.0, -5.0};
    case LookPreset::WarmFilm: return {0.05, 10.0, -5.0, 22.0, 4.0, 8.0, -8.0};
    case LookPreset::ColdFilm: return {-0.05, 12.0, -8.0, -24.0, -3.0, -5.0, -10.0};
    case LookPreset::Bleach: return {0.08, 28.0, -42.0, -4.0, 0.0, -14.0, 12.0};
    case LookPreset::SoftPortrait: return {0.12, -12.0, -4.0, 12.0, 6.0, 14.0, -18.0};
    case LookPreset::Night: return {-0.45, 20.0, -22.0, -30.0, -6.0, -20.0, -16.0};
    case LookPreset::Vintage: return {-0.08, 5.0, -24.0, 18.0, 8.0, 12.0, -15.0};
    default: return {};
    }
}

std::string TextPositionX(const TextOverlay& text) {
    if (text.position == TextPosition::Custom) return "(w-text_w)*" + Decimal(text.x);
    return "(w-text_w)/2";
}

std::string TextPositionY(const TextOverlay& text) {
    switch (text.position) {
    case TextPosition::Top: return "h*0.08";
    case TextPosition::Center: return "(h-text_h)/2";
    case TextPosition::Custom: return "(h-text_h)*" + Decimal(text.y);
    default: return "h-text_h-h*0.08";
    }
}

std::string FontFile(const TextOverlay& text) {
    if (text.font == "Segoe UI") return "C\\:/Windows/Fonts/segoeui.ttf";
    if (text.font == "Arial") return "C\\:/Windows/Fonts/arial.ttf";
    if (text.font == "Georgia") return "C\\:/Windows/Fonts/georgia.ttf";
    return "C\\:/Windows/Fonts/malgun.ttf";
}

std::string TextAlpha(const TextOverlay& text, double duration) {
    const double fadeIn = Clamp(text.fadeIn, 0.0, duration);
    const double fadeOut = Clamp(text.fadeOut, 0.0, duration);
    const double opacity = Clamp(text.opacity, 0.0, 1.0);
    if (fadeIn <= 0.0001 && fadeOut <= 0.0001) return Decimal(opacity);
    std::string expression;
    if (fadeIn > 0.0001 && fadeOut > 0.0001) {
        expression = "if(lt(t\\," + Decimal(fadeIn) + ")\\,t/" + Decimal(fadeIn)
            + "\\,if(gt(t\\," + Decimal(duration - fadeOut) + ")\\,(" + Decimal(duration) + "-t)/"
            + Decimal(fadeOut) + "\\,1))";
    } else if (fadeIn > 0.0001) {
        expression = "if(lt(t\\," + Decimal(fadeIn) + ")\\,t/" + Decimal(fadeIn) + "\\,1)";
    } else {
        expression = "if(gt(t\\," + Decimal(duration - fadeOut) + ")\\,(" + Decimal(duration) + "-t)/"
            + Decimal(fadeOut) + "\\,1)";
    }
    return Decimal(opacity) + "*(" + expression + ")";
}

std::string AudioFilter(const Clip& clip, double duration) {
    const auto& audio = clip.audio;
    std::vector<std::string> filters;
    filters.push_back("atrim=start=" + Decimal(clip.inPoint) + ":end=" + Decimal(clip.outPoint));
    filters.push_back("asetpts=PTS-STARTPTS");
    filters.push_back(BuildAtempoFilter(clip.speed));
    filters.push_back("aresample=48000");
    if (audio.normalize) filters.push_back("loudnorm=I=-16:TP=-1.5:LRA=11");
    if (std::abs(audio.eqLow) > 0.01) filters.push_back("bass=f=120:g=" + Decimal(audio.eqLow));
    if (std::abs(audio.eqMid) > 0.01) filters.push_back("equalizer=f=1000:t=q:w=1:g=" + Decimal(audio.eqMid));
    if (std::abs(audio.eqHigh) > 0.01) filters.push_back("treble=f=6000:g=" + Decimal(audio.eqHigh));
    const double gain = audio.muted ? 0.0 : std::pow(10.0, Clamp(audio.volumeDb, -60.0, 24.0) / 20.0);
    filters.push_back("volume=" + Decimal(gain));
    if (audio.fadeIn > 0.001) filters.push_back("afade=t=in:st=0:d=" + Decimal(std::min(audio.fadeIn, duration)));
    if (audio.fadeOut > 0.001) {
        const double fade = std::min(audio.fadeOut, duration);
        filters.push_back("afade=t=out:st=" + Decimal(std::max(0.0, duration - fade)) + ":d=" + Decimal(fade));
    }
    filters.push_back("aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo");

    std::string result;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (i) result += ',';
        result += filters[i];
    }
    return result;
}

std::string VideoFilter(const Clip& clip, int width, int height, double fps, bool includeTrim, bool deferHeavyEffects) {
    std::vector<std::string> filters;
    if (includeTrim) {
        filters.push_back("trim=start=" + Decimal(clip.inPoint) + ":end=" + Decimal(clip.outPoint));
        filters.push_back("setpts=(PTS-STARTPTS)/" + Decimal(clip.speed));
    }
    const std::string color = BuildColorFilter(clip.color, deferHeavyEffects);
    if (!color.empty()) filters.push_back(color);
    filters.push_back("scale=" + std::to_string(width) + ':' + std::to_string(height)
        + ":force_original_aspect_ratio=decrease:flags=bicubic");
    filters.push_back("pad=" + std::to_string(width) + ':' + std::to_string(height) + ":(ow-iw)/2:(oh-ih)/2:color=black");
    filters.push_back("setsar=1");
    if (fps > 0.0) filters.push_back("fps=" + Decimal(fps));
    const double duration = clip.timelineDuration();
    for (const auto& text : clip.texts) {
        if (text.text.empty()) continue;
        filters.push_back(
            "drawtext=fontfile='" + FontFile(text) + "':text='" + EscapeDrawText(text.text)
            + "':fontsize=" + Decimal(text.fontSize) + ":fontcolor=white:x='" + TextPositionX(text)
            + "':y='" + TextPositionY(text) + "':alpha='" + TextAlpha(text, duration)
            + "':shadowcolor=black@0.65:shadowx=2:shadowy=2");
    }
    std::string result;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (i) result += ',';
        result += filters[i];
    }
    return result;
}

} // namespace

AdaptiveProfile ChooseAdaptiveProfile(
    PerformanceMode mode,
    unsigned logicalProcessors,
    std::uint64_t physicalMemoryMb,
    const MediaItem* media) {
    AdaptiveProfile profile;
    const bool heavyMedia = media && (media->height > 1080 || media->width > 1920 || media->codec == "hevc" || media->codec == "h265");
    const bool lowEnd = logicalProcessors <= 2 || physicalMemoryMb < 6000;
    const bool midRange = logicalProcessors <= 4 || physicalMemoryMb < 10000;

    if (mode == PerformanceMode::Quality) {
        profile = {1920, 1080, static_cast<int>(std::max(1U, logicalProcessors / 2U)), false, false,
            "품질 우선", "원본에 가까운 미리보기 품질을 유지합니다."};
    } else if (mode == PerformanceMode::Speed) {
        profile = {640, 360, 1, heavyMedia, true,
            "속도 우선", "미리보기와 임시 효과의 부하를 가장 낮춥니다."};
    } else if (mode == PerformanceMode::Balanced) {
        profile = {960, 540, static_cast<int>(std::max(1U, logicalProcessors / 3U)), heavyMedia, false,
            "균형", "화질과 반응성을 균형 있게 유지합니다."};
    } else if (lowEnd) {
        profile = {640, 360, 1, heavyMedia, true,
            "자동 · 저사양", "CPU 코어 또는 메모리가 적어 가벼운 미리보기를 사용합니다."};
    } else if (midRange || heavyMedia) {
        profile = {960, 540, static_cast<int>(std::max(1U, logicalProcessors / 3U)), heavyMedia, false,
            "자동 · 균형", heavyMedia ? "고해상도 또는 H.265 미디어를 감지했습니다." : "현재 시스템에 맞춰 균형 모드를 사용합니다."};
    } else {
        profile = {1280, 720, static_cast<int>(std::max(2U, logicalProcessors / 2U)), false, false,
            "자동 · 고품질", "현재 시스템에서 고품질 미리보기가 가능합니다."};
    }
    return profile;
}

ProbeInfo ParseProbeOutput(std::string_view output) {
    ProbeInfo result;
    std::istringstream stream{std::string(output)};
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        try {
            if (key == "codec_name" && result.codec.empty()) result.codec = value;
            else if (key == "width") result.width = std::stoi(value);
            else if (key == "height") result.height = std::stoi(value);
            else if (key == "avg_frame_rate" || key == "r_frame_rate") {
                const double fps = ParseFraction(value);
                if (fps > 0.0) result.fps = fps;
            } else if (key == "duration") {
                const double duration = std::stod(value);
                if (duration > result.duration) result.duration = duration;
            } else if (key == "codec_type" && value == "audio") result.hasAudio = true;
        } catch (...) {
            // A malformed optional field should not discard the other probe data.
        }
    }
    result.valid = result.width > 0 && result.height > 0 && result.duration > 0.0;
    return result;
}

std::vector<std::string> BuildProbeArguments(const std::filesystem::path& input) {
    return {
        "-v", "error",
        "-show_entries", "stream=codec_type,codec_name,width,height,avg_frame_rate,duration:format=duration",
        "-of", "default=noprint_wrappers=1",
        PathToUtf8(input)
    };
}

std::vector<std::string> BuildProxyArguments(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const AdaptiveProfile& profile) {
    return {
        "-hide_banner", "-loglevel", "error", "-y",
        "-i", PathToUtf8(input),
        "-map", "0:v:0", "-map", "0:a?",
        "-vf", "scale=" + std::to_string(profile.previewWidth) + ":-2:force_original_aspect_ratio=decrease:flags=fast_bilinear",
        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "28", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart",
        PathToUtf8(output)
    };
}

std::vector<std::string> BuildPreviewStillArguments(
    const MediaItem& media,
    const Clip& clip,
    double sourcePosition,
    const std::filesystem::path& outputBmp,
    int width,
    int height,
    bool deferHeavyEffects) {
    const double position = Clamp(sourcePosition, clip.inPoint, std::max(clip.inPoint, clip.outPoint - 0.001));
    return {
        "-hide_banner", "-loglevel", "error", "-y",
        "-ss", Decimal(position), "-i", PathToUtf8(media.path),
        "-frames:v", "1", "-an",
        "-vf", VideoFilter(clip, width, height, 0.0, false, deferHeavyEffects),
        "-c:v", "bmp", PathToUtf8(outputBmp)
    };
}

ExportPlan BuildExportPlan(
    const Project& project,
    const std::filesystem::path& filterScriptPath,
    const std::filesystem::path& outputPath) {
    ExportPlan plan;
    if (project.timeline.empty()) {
        plan.warnings.push_back("타임라인이 비어 있습니다.");
        return plan;
    }

    plan.arguments = {"-hide_banner", "-y"};
    for (const auto& clip : project.timeline) {
        const MediaItem* media = project.findMedia(clip.mediaId);
        if (!media) {
            plan.warnings.push_back("원본이 없는 클립을 발견했습니다.");
            continue;
        }
        plan.arguments.push_back("-i");
        plan.arguments.push_back(PathToUtf8(media->path));
    }
    if (!plan.warnings.empty()) return plan;

    double outputFps = project.output.fps;
    if (outputFps <= 0.0) {
        if (const MediaItem* first = project.findMedia(project.timeline.front().mediaId)) outputFps = first->fps;
        if (outputFps <= 0.0) outputFps = 30.0;
    }

    std::string concatInputs;
    for (std::size_t index = 0; index < project.timeline.size(); ++index) {
        const Clip& clip = project.timeline[index];
        const MediaItem* media = project.findMedia(clip.mediaId);
        const std::string number = std::to_string(index);
        plan.filterComplex += '[' + number + ":v]" + VideoFilter(
            clip, project.output.width, project.output.height, outputFps, true, false) + "[v" + number + "];\n";
        if (project.output.preserveAudio) {
            if (media && media->hasAudio) {
                plan.filterComplex += '[' + number + ":a]" + AudioFilter(clip, clip.timelineDuration()) + "[a" + number + "];\n";
            } else {
                plan.filterComplex += "anullsrc=channel_layout=stereo:sample_rate=48000,atrim=duration="
                    + Decimal(clip.timelineDuration()) + "[a" + number + "];\n";
            }
        }
        concatInputs += "[v" + number + ']';
        if (project.output.preserveAudio) concatInputs += "[a" + number + ']';
    }
    plan.filterComplex += concatInputs + "concat=n=" + std::to_string(project.timeline.size())
        + (project.output.preserveAudio ? ":v=1:a=1[vout][aout]\n" : ":v=1:a=0[vout]\n");

    plan.arguments.insert(plan.arguments.end(), {
        "-filter_complex_script", PathToUtf8(filterScriptPath),
        "-map", "[vout]"
    });
    if (project.output.preserveAudio) plan.arguments.insert(plan.arguments.end(), {"-map", "[aout]"});

    const char* preset = "veryfast";
    if (project.performanceMode == PerformanceMode::Quality) preset = "medium";
    else if (project.performanceMode == PerformanceMode::Balanced) preset = "fast";
    else if (project.performanceMode == PerformanceMode::Speed) preset = "ultrafast";
    const char* crf = "18";
    if (project.output.quality == ExportQuality::Standard) crf = "21";
    else if (project.output.quality == ExportQuality::Compact) crf = "26";

    if (project.output.codec == VideoCodec::H265) {
        plan.arguments.insert(plan.arguments.end(), {"-c:v", "libx265", "-tag:v", "hvc1"});
    } else {
        plan.arguments.insert(plan.arguments.end(), {"-c:v", "libx264"});
    }
    plan.arguments.insert(plan.arguments.end(), {
        "-preset", preset, "-crf", crf, "-pix_fmt", "yuv420p"
    });
    if (project.output.preserveAudio) plan.arguments.insert(plan.arguments.end(), {"-c:a", "aac", "-b:a", "192k"});
    plan.arguments.insert(plan.arguments.end(), {"-movflags", "+faststart", PathToUtf8(outputPath)});
    return plan;
}

std::string BuildColorFilter(const ColorGrade& color, bool deferHeavyEffects) {
    const LookAdjustment look = LookFor(color.look);
    const double strength = Clamp(color.lookStrength, 0.0, 100.0) / 100.0;
    const double exposure = Clamp(color.exposure + look.exposure * strength, -5.0, 5.0);
    const double contrast = Clamp(color.contrast + look.contrast * strength, -100.0, 100.0);
    const double saturation = Clamp(color.saturation + look.saturation * strength, 0.0, 200.0);
    const double temperature = Clamp(color.temperature + look.temperature * strength, -100.0, 100.0);
    const double tint = Clamp(color.tint + look.tint * strength, -100.0, 100.0);
    const double shadows = Clamp(color.shadows + look.shadows * strength + color.curveShadows, -150.0, 150.0);
    const double highlights = Clamp(color.highlights + look.highlights * strength + color.curveHighlights, -150.0, 150.0);

    std::vector<std::string> filters;
    if (std::abs(exposure) > 0.0001) filters.push_back("exposure=exposure=" + Decimal(exposure) + ":black=0");

    double y0 = Clamp(color.blacks * 0.0015, 0.0, 0.2);
    double y1 = Clamp(0.25 + shadows * 0.0016, y0 + 0.02, 0.48);
    double y2 = Clamp(0.5 + color.curveMidtones * 0.0016, y1 + 0.02, 0.73);
    double y3 = Clamp(0.75 + highlights * 0.0016, y2 + 0.02, 0.98);
    double y4 = Clamp(1.0 + color.whites * 0.0015, y3 + 0.02, 1.0);
    if (std::abs(y0) > 0.0001 || std::abs(y1 - 0.25) > 0.0001 || std::abs(y2 - 0.5) > 0.0001
        || std::abs(y3 - 0.75) > 0.0001 || std::abs(y4 - 1.0) > 0.0001) {
        filters.push_back("curves=all='0/" + Decimal(y0) + " 0.25/" + Decimal(y1) + " 0.5/" + Decimal(y2)
            + " 0.75/" + Decimal(y3) + " 1/" + Decimal(y4) + "'");
    }
    filters.push_back("eq=contrast=" + Decimal(std::max(0.05, 1.0 + contrast / 100.0))
        + ":saturation=" + Decimal(saturation / 100.0));

    double rs = temperature / 220.0 + tint / 440.0 + color.shadowWheel[0] / 500.0;
    double gs = -tint / 260.0 + color.shadowWheel[1] / 500.0;
    double bs = -temperature / 220.0 + tint / 440.0 + color.shadowWheel[2] / 500.0;
    double rm = temperature / 280.0 + tint / 560.0 + color.midtoneWheel[0] / 500.0;
    double gm = -tint / 320.0 + color.midtoneWheel[1] / 500.0;
    double bm = -temperature / 280.0 + tint / 560.0 + color.midtoneWheel[2] / 500.0;
    double rh = temperature / 340.0 + tint / 680.0 + color.highlightWheel[0] / 500.0;
    double gh = -tint / 380.0 + color.highlightWheel[1] / 500.0;
    double bh = -temperature / 340.0 + tint / 680.0 + color.highlightWheel[2] / 500.0;
    for (double* value : {&rs, &gs, &bs, &rm, &gm, &bm, &rh, &gh, &bh}) *value = Clamp(*value, -1.0, 1.0);
    if (std::abs(rs) + std::abs(gs) + std::abs(bs) + std::abs(rm) + std::abs(gm) + std::abs(bm)
        + std::abs(rh) + std::abs(gh) + std::abs(bh) > 0.001) {
        filters.push_back("colorbalance=rs=" + Decimal(rs) + ":gs=" + Decimal(gs) + ":bs=" + Decimal(bs)
            + ":rm=" + Decimal(rm) + ":gm=" + Decimal(gm) + ":bm=" + Decimal(bm)
            + ":rh=" + Decimal(rh) + ":gh=" + Decimal(gh) + ":bh=" + Decimal(bh));
    }
    if (!deferHeavyEffects && color.grain > 0.1) {
        filters.push_back("noise=alls=" + Decimal(Clamp(color.grain, 0.0, 100.0) * 0.24) + ":allf=t+u");
    }
    if (!deferHeavyEffects && color.vignette > 0.1) {
        const double angle = 0.15 + Clamp(color.vignette, 0.0, 100.0) / 100.0 * 0.75;
        filters.push_back("vignette=angle=" + Decimal(angle));
    }
    if (color.letterbox > 0.1) {
        const double fraction = Clamp(color.letterbox, 0.0, 20.0) / 100.0;
        filters.push_back("drawbox=x=0:y=0:w=iw:h=ih*" + Decimal(fraction) + ":color=black:t=fill");
        filters.push_back("drawbox=x=0:y=ih-ih*" + Decimal(fraction) + ":w=iw:h=ih*" + Decimal(fraction) + ":color=black:t=fill");
    }

    std::string result;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (i) result += ',';
        result += filters[i];
    }
    return result;
}

std::string BuildAtempoFilter(double speed) {
    speed = Clamp(speed, 0.1, 8.0);
    std::vector<double> factors;
    while (speed > 2.0 + 0.0001) {
        factors.push_back(2.0);
        speed /= 2.0;
    }
    while (speed < 0.5 - 0.0001) {
        factors.push_back(0.5);
        speed /= 0.5;
    }
    factors.push_back(speed);
    std::string result;
    for (std::size_t i = 0; i < factors.size(); ++i) {
        if (i) result += ',';
        result += "atempo=" + Decimal(factors[i]);
    }
    return result;
}

std::string EscapeDrawText(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '\'': escaped += "\\'"; break;
        case ':': escaped += "\\:"; break;
        case '%': escaped += "\\%"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': break;
        default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

} // namespace fillema
