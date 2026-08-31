# Febius Fillema

**빠르고 정교한 시네마틱 영상 편집 도구.**

Fillema는 기능을 끝없이 쌓는 범용 편집기가 아닙니다. 자주 쓰는 컷 편집, 시네마틱 색보정, 오디오, 텍스트, 속도 조절과 출력만 남기고 실행 속도와 반응성을 우선하는 Windows 영상 편집기입니다.

> 고사양 컴퓨터가 아니라 편집자의 판단이 결과물을 결정해야 한다.

## v0.1에서 되는 것

- 미디어 다중 가져오기와 파일 드래그 즉시 타임라인 배치
- 자르기, 현재 위치 분할, 리플 트림, 리플 삭제, 복제, 복사·붙여넣기, 드래그 재정렬
- 노출, 대비, 하이라이트, 그림자, 화이트, 블랙, 색온도, 틴트, 채도
- 3점 RGB 커브와 Shadow/Midtone/Highlight RGB 휠
- Cinema Neutral, Warm Film, Cold Film, Bleach, Soft Portrait, Night, Vintage 룩과 강도
- Film Grain, Vignette, Letterbox
- 속도 0.1×~8×
- 볼륨, 페이드, 음소거, 3밴드 EQ, -16 LUFS 노멀라이즈
- 제목·자막, 글꼴, 위치, 크기, 불투명도, 페이드
- H.264/H.265 MP4 출력
- `.fillema` 프로젝트, 30초 자동 저장, 50단계 실행 취소
- `Ctrl+P` 명령 검색

## Adaptive Editing

기본 성능 모드는 **자동**입니다. CPU 코어 수, 메모리, 해상도와 코덱을 보고 미리보기 해상도·작업 스레드·성능용 편집본 생성을 정합니다.

4K 또는 H.265 원본이 현재 PC에 무거우면 백그라운드에서 360p/540p 성능용 편집본을 만듭니다. 타임라인 재생은 이 파일을 쓰지만 최종 출력은 언제나 원본을 사용합니다. 프록시라는 용어를 몰라도 되는 구조입니다.

색보정 값을 움직이면 FFmpeg가 실제 출력 필터와 같은 정지 프레임을 백그라운드에서 만듭니다. 재생 중에는 반응성을 위해 가벼운 원본/성능용 화면을 보여주고, 정지하면 정확 색감 화면으로 전환합니다.

## 실행

릴리스에는 두 파일이 있습니다.

- `Fillema-v0.1-Windows-x64.zip`: FFmpeg를 포함해 바로 쓰는 전체판
- `Fillema-v0.1-Windows-x64-lite.zip`: 이미 FFmpeg가 설치되어 PATH에 잡힌 PC용 초경량판

Windows 10/11 64비트를 지원합니다. 압축을 풀고 `Fillema.exe`를 실행하면 됩니다. 설치 과정이나 새 프로젝트 설정 창은 없습니다.

## 핵심 단축키

| 키 | 동작 |
|---|---|
| `Space` | 재생 / 일시정지 |
| `S`, `Ctrl+K` | 현재 위치 분할 |
| `Delete` | 리플 삭제 |
| `Q` / `W` | 앞 / 뒤 리플 트림 |
| `C` | 기본 색보정 |
| `T` | 텍스트 추가 |
| `Ctrl+P` | 명령 검색 |
| `Ctrl+E` | 영상 출력 |
| `F` | 전체 화면 미리보기 |

전체 목록은 [docs/SHORTCUTS.md](docs/SHORTCUTS.md)에 있습니다.

## 현재 한계

v0.1은 제품 철학과 편집 흐름을 검증하는 첫 Core 릴리스입니다.

- 타임라인은 순차 단일 영상 트랙입니다.
- 재생 중 실시간 색처리 대신, 멈춘 화면에서 출력과 같은 색을 확인합니다.
- 전환 효과, 멀티캠, 모션그래픽, 플러그인, AI 기능은 없습니다.
- Windows N 에디션은 Media Feature Pack이 필요할 수 있습니다.

이 제한은 무거운 기능을 섣불리 넣지 않기 위한 의도적인 선택입니다.

## 빌드

Visual Studio 2022와 CMake 3.24 이상이 필요합니다. 외부 GUI 라이브러리는 없습니다.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

미디어 분석과 출력에는 `tools/ffmpeg.exe`, `tools/ffprobe.exe` 및 같은 폴더의 FFmpeg DLL이 필요합니다. 없으면 PATH에서 찾습니다.

## 구조

```text
Native Win32 UI
├─ Media Foundation preview
├─ Timeline + project model
├─ Adaptive background workers
└─ FFmpeg filter / render planner
```

자세한 내부 구조는 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)를 참고하세요.

