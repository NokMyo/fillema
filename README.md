# Febius Fillema

**빠르고 정교한 시네마틱 영상 편집 도구.**

Fillema는 Febius Creator Series의 초경량 네이티브 영상 편집기입니다. 기능의 수보다 실행 속도, 반응성, 낮은 자원 사용량, 정밀한 편집 경험을 우선합니다.

## Product principles

- **Instant** — 실행과 조작은 즉각적이어야 합니다.
- **Efficient** — 같은 결과라면 더 적은 CPU, 메모리, 저장공간을 사용합니다.
- **Precise** — 빠르다는 이유로 편집 정밀도를 희생하지 않습니다.
- **Accessible** — 고사양 하드웨어를 필수 조건으로 만들지 않습니다.

> 고사양 컴퓨터가 아니라 편집자의 판단이 결과물을 결정해야 합니다.

## 0.1 목표

첫 단계는 외부 UI 프레임워크 없이 Win32로 동작하는 편집기 셸입니다.

- 네이티브 Windows UI
- 미디어 / 미리보기 / 타임라인 / 속성의 4영역 작업공간
- 프로젝트 새로 만들기, 열기, 저장
- 미디어 파일 등록
- 타임라인 클립 배치
- 재생/정지 상태 및 플레이헤드
- 최소 메모리 사용을 위한 경량 프로젝트 모델
- FFmpeg 연동을 고려한 분리형 미디어 엔진 인터페이스

## Build

요구 사항:

- Windows 10 이상
- Visual Studio 2022 Build Tools 또는 Visual Studio 2022
- CMake 3.20 이상

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

실행 파일은 `build/Release/Fillema.exe`에 생성됩니다.

## Architecture

```text
Fillema UI (Win32)
    ↓
Application / Project model
    ↓
Timeline engine
    ↓
Media engine interface
    ↓
Decode / Effects / Color / Audio / Encode
```

0.1 단계에서는 UI와 프로젝트/타임라인 구조를 먼저 고정하고, 실제 영상 디코딩·인코딩은 다음 단계에서 FFmpeg 기반으로 연결합니다.

## License

Copyright © Febius / nokmyo. All rights reserved.
