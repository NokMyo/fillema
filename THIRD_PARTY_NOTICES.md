# Third-party notices

Fillema itself does not link against FFmpeg libraries. The full release archive aggregates a separate FFmpeg command-line distribution and invokes it as an independent process for media analysis and rendering.

## FFmpeg

- Project: https://ffmpeg.org/
- Binary distributor: https://github.com/BtbN/FFmpeg-Builds
- Build flavor: Windows x64 GPL shared build (includes the x264/x265 encoders used by Fillema)
- License information: https://ffmpeg.org/legal.html
- Corresponding source: https://github.com/FFmpeg/FFmpeg

The FFmpeg license files supplied by its binary distribution are copied into the release package under `tools/licenses` when present.
