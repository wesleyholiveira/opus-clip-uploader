
_AI Translated_


---

# OBS Clip Cropper Plugin

Plugin for **OBS Studio 30.1+** that helps send recordings and livestreams to **Opus Clip**, with support for video editing/review, local transcription with **Whisper.cpp/GGML**, context generation with **OpenAI/GPT**, and background upload progress.

## Table of Contents

- [Supported OBS Version](#supported-obs-version)
- [Features](#features)
  - [Opus Clip Upload](#opus-clip-upload)
  - [Review and Clip Range Selection](#review-and-clip-range-selection)
  - [Video Editing and Review](#video-editing-and-review)
  - [Automatic Opus Curation](#automatic-opus-curation)
  - [Local Transcription with Whisper.cpp/GGML](#local-transcription-with-whispercppggml)
  - [OpenAI/GPT Integration](#openaigpt-integration)
  - [Settings](#settings)
  - [Localization](#localization)
  - [Bundled FFmpeg](#bundled-ffmpeg)
- [Whisper/GGML Models](#whisperggml-models)
  - [Exact Model File Names](#exact-model-file-names)
  - [Where to Place the Models](#where-to-place-the-models)
- [Project Folder Structure](#project-folder-structure)
- [How to Build and Run on Windows](#how-to-build-and-run-on-windows)
- [How to Manually Install on OBS for Windows](#how-to-manually-install-on-obs-for-windows)
- [How to Build and Run on Linux](#how-to-build-and-run-on-linux)
- [How to Check Whether CUDA Is Being Used](#how-to-check-whether-cuda-is-being-used)
- [Main Usage Flow](#main-usage-flow)
- [Troubleshooting](#troubleshooting)
- [Quick Windows Commands](#quick-windows-commands)

## Supported OBS Version

This plugin was developed and tested for **OBS Studio 30.1+**.

Older OBS versions may not provide compatible APIs, directory layout, or dependencies required by the plugin.

## Features

### Opus Clip Upload

The plugin allows you to send recordings or selected video ranges to Opus Clip.

Main features:

- upload videos to Opus Clip;
- upload progress bar;
- upload progress displayed in a separate, non-modal, minimizable window;
- cancellation through the **Cancel** button, **Esc** key, or window **X** button;
- support for uploading multiple videos;
- support for selected ranges from the editor/review step before upload;
- support for automatic curation or fixed-range upload.

### Review and Clip Range Selection

Before uploading, the plugin allows you to review the video and select the ranges that will be sent to Opus.

Main features:

- review dialog before upload;
- video editor with timeline;
- range selection with start and end timestamps;
- clicking a marker repositions the video;
- immediate seek on the timeline;
- fullscreen support;
- range validation before upload;
- integration with local transcription and GPT prompt/context generation.

If no range is selected, the plugin may use a default initial range to avoid sending an empty selection.

### Video Editing and Review

The plugin includes a video editing/review screen for selecting the video ranges that will be sent to Opus Clip.

This step defines whether Opus will receive **a fixed clip range** or **a larger curation window**.

When `Skip Curate` is disabled, the selected range works as a search window where Opus can automatically find good clips.

When `Skip Curate` is enabled, the selected range is treated as a fixed clip.

#### Video Editor Features

The video editing/review screen includes:

- integrated video player;
- timeline with immediate seek;
- play/pause control;
- start and end range selection;
- selected range visualization;
- marker click to reposition the video;
- support for long ranges used as automatic curation windows;
- fullscreen support;
- controlled playback/loop behavior during review;
- integration with transcription and GPT-generated prompt before upload;
- range validation before upload;
- upload flow continuation after accepting the review.

#### How to Select a Clip Range

Recommended flow:

1. Open the video in the editor/review screen.
2. Use the timeline to navigate to the start position.
3. Mark the range start.
4. Navigate to the end position.
5. Mark the range end.
6. Review the selected range.
7. Confirm the upload to Opus.

Example:

- **Start:** `10min`
- **End:** `2h`

With `Skip Curate` disabled, this means:

> Opus, find good clips inside the interval from 10min to 2h.

With `Skip Curate` enabled, this means:

> Use exactly the range from 10min to 2h as a fixed clip.

#### Useful Editor Actions and Shortcuts

Shortcuts may depend on the current focused element inside the window, but the editor is designed around the following review actions:

| Action | Behavior |
|---|---|
| Play/Pause | Starts or pauses video playback |
| Click timeline | Moves the video to the clicked position |
| Drag timeline | Seeks to the desired position |
| Click marker | Repositions the video to the selected marker |
| Fullscreen button | Toggles the video fullscreen view |
| Confirm review | Continues to transcription/GPT/upload |
| Cancel review | Closes the review without uploading to Opus |

#### Recommendations for Better Clips

To improve the quality of clips generated by Opus:

- select ranges with enough content for curation;
- avoid ranges that are too short when you want multiple clips;
- use a larger range when you want Opus to find the best moments;
- keep `Skip Curate` disabled for automatic curation;
- use `Skip Curate` only when you want to upload a fixed clip;
- avoid selecting sections with too much silence, static screen, or conversation unrelated to the main subject.

#### Relationship with Transcription and GPT

Before sending the video to Opus, the plugin can run this flow:

```txt
selected video
↓
FFmpeg extracts audio
↓
Whisper/GGML transcribes locally
↓
transcription is saved to cache
↓
GPT generates curation prompt/context
↓
review/upload continues
```

The generated prompt helps Opus understand which clips should be prioritized.

The default prompt instructs that each clip should have **a beginning, middle, and end**, and should focus on **exactly one main subject**.

It also instructs the model to remove unrelated parts, such as repetitive comments about delayed chat, PIX, stars, donations, or reading messages, unless that is the main subject of the clip.

### Automatic Opus Curation

The plugin supports two main modes: **active curation** and **Skip Curate**.

#### Active Curation

When `Skip Curate` is disabled, the selected range is treated as a **search window**.

In this mode, the plugin sends the interval to Opus as a curation area, and Opus decides the best clips inside that range.

In this mode, the plugin does not force:

- `clip_start`
- `clip_duration`
- `clipDurations`

#### Skip Curate

When `Skip Curate` is enabled, the selected range is treated as a **fixed clip**.

In this mode, the plugin sends the exact start time and duration to Opus.

### Local Transcription with Whisper.cpp/GGML

The plugin can locally transcribe video audio before sending context to GPT/Opus.

Features:

- audio extraction from video using FFmpeg;
- local transcription using Whisper.cpp;
- support for GGML models;
- CUDA/GPU backend support when enabled in the build;
- transcription cache per video;
- detailed logs to validate whether CUDA was requested and whether Whisper loaded correctly;
- cancellation during transcription;
- progress displayed in a non-modal, minimizable window.

Current flow:

```txt
video
↓
FFmpeg extracts audio
↓
Whisper.cpp/GGML transcribes
↓
TranscriptStore saves cache
```

### OpenAI/GPT Integration

The plugin can automatically generate context/prompt before review/upload.

Features:

- automatic prompt generation before review/upload;
- generated prompt cache per video;
- configurable OpenAI model;
- OpenAI model automatically disabled when the Whisper/GGML model is not found;
- editable input template sent to GPT from the Settings menu;
- default template saved in `PluginConfig`;
- template can be edited by the user.

The default prompt instructs GPT to:

- suggest clips with a beginning, middle, and end;
- keep exactly one main subject per clip;
- avoid mixing different subjects in the same clip;
- remove parts unrelated to the main subject;
- ignore repetitive sections about delayed chat, PIX, stars, donations, super chat, or reading messages, unless that is the main subject of the clip.

### Settings

The plugin includes a settings screen with:

- Opus API key;
- OpenAI settings;
- OpenAI model selection;
- Whisper/GGML model selection/configuration;
- GPT input template;
- curation preferences;
- localized fields in `pt-BR` and `en-US`.

When the Whisper/GGML model is not found, the plugin may automatically save:

```txt
openai_model = disabled
```

This prevents the user from assuming GPT/transcription is enabled when the local model is missing.

### Localization

The plugin includes locale files for:

- `pt-BR`
- `en-US`

Files:

```txt
data/locale/pt-BR.ini
data/locale/en-US.ini
```

### Bundled FFmpeg

The plugin uses FFmpeg to extract audio from videos before transcription.

The build can automatically prepare the FFmpeg runtime using:

```bat
node .github\scripts\prepare-ffmpeg-runtime.mjs
```

In the installed package, FFmpeg is located at:

```txt
data/obs-plugins/clip-cropper/ffmpeg/
```

On Windows:

```txt
data/obs-plugins/clip-cropper/ffmpeg/ffmpeg.exe
```

On Linux:

```txt
share/obs/obs-plugins/clip-cropper/ffmpeg/ffmpeg
```

## Whisper/GGML Models

Models are **not automatically bundled** in the final build because they are large and optional files.

The `models` folder is created in the package with only a `.gitkeep` file.

### Exact Model File Names

The file name must match the model selected in the plugin. Otherwise, the plugin will consider the model missing and may automatically disable the OpenAI model with:

```txt
openai_model = disabled
```

Use exactly one of these file names:

- `ggml-tiny.bin`
- `ggml-base.bin`
- `ggml-small.bin`
- `ggml-medium.bin`
- `ggml-large-v3.bin`

Valid example:

- `ggml-base.bin`

Invalid examples:

- `base.bin`
- `whisper-base.bin`
- `ggml-base.en.bin`
- `ggml_base.bin`
- `ggml-large.bin`
- `large-v3.bin`

The plugin searches for the file by its exact name.

If `tiny` is selected in the settings, the expected file is `ggml-tiny.bin`.

If `base` is selected in the settings, the expected file is `ggml-base.bin`.

If `small` is selected in the settings, the expected file is `ggml-small.bin`.

If `medium` is selected in the settings, the expected file is `ggml-medium.bin`.

If `large-v3` is selected in the settings, the expected file is `ggml-large-v3.bin`.

### Where to Place the Models

#### Recommended Path in the Local Windows Package

After running local install:

```txt
package\data\obs-plugins\clip-cropper\models
```

Example:

```txt
package\data\obs-plugins\clip-cropper\models\ggml-base.bin
```

#### Recommended Path in a Real OBS Installation on Windows

If you copy the contents of `package` into the OBS installation folder, the expected path is:

```txt
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\models
```

Example:

```txt
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\models\ggml-base.bin
```

#### Linux Path

In Linux/DEB packages, the expected path is:

```txt
/usr/share/obs/obs-plugins/clip-cropper/models
```

Example:

```txt
/usr/share/obs/obs-plugins/clip-cropper/models/ggml-base.bin
```

#### Legacy Fallbacks

The plugin may also search legacy paths, such as:

```txt
%APPDATA%\obs-studio\plugins\clip-cropper\models
%APPDATA%\obs-studio\plugins\clip-cropper\data\models
```

However, the recommended path is the plugin data directory:

```txt
data/obs-plugins/clip-cropper/models
```

## Project Folder Structure

General structure:

```txt
.
├── CMakeLists.txt
├── CMakePresets.json
├── data/
├── deps/
├── cmake/
├── src/
├── .github/
└── README.md
```

### `src/`

Main plugin source code.

```txt
src/
├── auth/
├── gpt/
├── models/
├── opus/
├── transcription/
├── ui/
├── utils/
└── worker/
```

#### `src/auth/`

Authentication and OAuth-related code.

Contains, for example:

```txt
google-oauth.cpp
google-oauth.hpp
oauth-callback-server.cpp
oauth-callback-server.hpp
```

Responsible for authentication flows, local callback server, and integration with external services when needed.

#### `src/gpt/`

OpenAI/GPT integration code.

Contains:

```txt
gpt-prompt-client.cpp
gpt-prompt-client.hpp
gpt-prompt-store.cpp
gpt-prompt-store.hpp
```

Responsibilities:

- build the input sent to GPT;
- apply the configurable prompt template;
- send requests to OpenAI;
- save generated prompts to cache;
- retrieve saved prompts per video.

#### `src/models/`

Simple domain objects and structures.

Contains, for example:

```txt
curation-settings.hpp
transcript.hpp
```

Responsible for representing:

- curation settings;
- transcript;
- transcribed segments;
- selected ranges.

#### `src/opus/`

Opus Clip integration client.

Contains:

```txt
opus-clip-client.cpp
opus-clip-client.hpp
```

Responsibilities:

- create upload/project on Opus;
- send curation metadata;
- differentiate a range used as a curation window from a fixed clip;
- build the Opus API payload.

#### `src/transcription/`

Local transcription code.

Contains:

```txt
realtime-transcription-service.cpp
realtime-transcription-service.hpp
transcript-store.cpp
transcript-store.hpp
```

Despite the `realtime-transcription-service` name, the current flow no longer uses OBS raw audio callbacks.

The service currently does:

```txt
video
↓
FFmpeg extracts audio
↓
Whisper.cpp transcribes
↓
transcript cache
```

Responsibilities:

- resolve the Whisper/GGML model;
- load Whisper.cpp;
- process extracted audio;
- generate transcript segments;
- save and retrieve transcript cache.

#### `src/ui/`

Qt UI code.

Contains:

```txt
ui.cpp
ui-common.cpp
ui-common.hpp
settings-dialog.cpp
upload-confirm-dialog.cpp
upload-flow.cpp
upload-review-dialog.cpp
video-editor-actions.cpp
video-marker-editor.cpp
gpt-review-prompt.cpp
advanced-settings-tree.cpp
```

Responsibilities:

- settings screen;
- confirmation dialogs;
- progress dialogs;
- review before upload;
- video editor and markers;
- visual integration with OBS;
- minimizable transcription/GPT/upload progress windows.

#### `src/utils/`

Utility functions.

Contains:

```txt
config.cpp
config.hpp
file.cpp
file.hpp
request.cpp
request.hpp
```

Responsibilities:

- read/write configuration through `PluginConfig`;
- file helpers;
- HTTP request helpers.

#### `src/worker/`

Asynchronous workers.

Contains:

```txt
upload-worker.cpp
upload-worker.hpp
```

Responsible for executing uploads without blocking the main OBS UI.

### `data/`

Data files bundled with the plugin.

```txt
data/
├── locale/
└── models/
```

#### `data/locale/`

Localization files:

```txt
en-US.ini
pt-BR.ini
```

#### `data/models/`

Expected folder for Whisper/GGML models.

In the repository and in the package, it should contain only:

```txt
.gitkeep
```

Model `.bin` files should not be versioned or automatically bundled.

### `deps/`

Vendored or external project dependencies.

```txt
deps/
└── whisper.cpp/
```

The main dependency used here is `whisper.cpp`, including `ggml`.

### `cmake/`

Auxiliary build scripts.

```txt
cmake/
├── common/
├── linux/
├── macos/
└── windows/
```

Responsible for:

- dependency bootstrap;
- platform defaults;
- install helpers;
- packaging;
- integration with the OBS Plugin Template.

### `.github/`

CI/CD configuration.

```txt
.github/
├── actions/
├── scripts/
└── workflows/
```

#### `.github/actions/`

Composite actions used by workflows.

Includes actions such as:

```txt
build-plugin
run-clang-format
check-changes
```

#### `.github/scripts/`

Auxiliary scripts.

Example:

```txt
prepare-ffmpeg-runtime.mjs
Build-Windows.ps1
```

Responsible for preparing the FFmpeg runtime and helping local/CI builds.

#### `.github/workflows/`

GitHub Actions workflows.

Responsible for:

- Windows build;
- Linux build;
- macOS build;
- clang-format;
- packaging;
- release artifacts.

## How to Build and Run on Windows

### Requirements

- Windows 10/11
- OBS Studio 30.1+
- CMake
- Ninja
- Visual Studio Build Tools/MSVC
- Node.js
- CUDA Toolkit, if using Whisper with GPU
- Qt from the OBS/plugin dependencies

### Prepare FFmpeg Runtime

```bat
node .github\scripts\prepare-ffmpeg-runtime.mjs
```

### Set the FFmpeg Variable

```bat
set "CLIP_CROPPER_FFMPEG_RUNTIME_DIR=%CD%\.ffmpeg-runtime"
```

### Configure with the Ninja CUDA Preset

```bat
cmake --preset windows-ninja-cuda-x64 -DCLIP_CROPPER_FFMPEG_RUNTIME_DIR="%CLIP_CROPPER_FFMPEG_RUNTIME_DIR%"
```

### Build

```bat
cmake --build --preset windows-ninja-cuda-x64 --parallel
```

### Install into the Local `package` Directory

```bat
cmake --install build_cuda --prefix "%CD%\package"
```

### Validate Main Files

```bat
dir "%CD%\package\obs-plugins\64bit\clip-cropper.dll"
```

```bat
dir "%CD%\package\data\obs-plugins\clip-cropper\ffmpeg\ffmpeg.exe"
```

```bat
dir "%CD%\package\data\obs-plugins\clip-cropper\models"
```

The `models` folder should contain only `.gitkeep` until you manually copy the GGML model.

## How to Manually Install on OBS for Windows

After generating the local package:

```txt
package\
```

Copy its contents into the OBS installation root.

Example destination:

```txt
C:\Program Files\obs-studio\
```

The final structure should look like this:

```txt
C:\Program Files\obs-studio\obs-plugins\64bit\clip-cropper.dll
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\locale\pt-BR.ini
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\locale\en-US.ini
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\ffmpeg\ffmpeg.exe
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\models\ggml-base.bin
```

## How to Build and Run on Linux

### Build

General flow:

```bash
cmake -S . -B build_x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_x86_64 --parallel
```

### Package

```bash
cmake --build build_x86_64 --target package
```

### Note About CUDA on Linux

When Whisper CUDA is enabled, the plugin may depend on NVIDIA driver libraries, such as `libcuda.so.1`.

This library comes from the NVIDIA driver installed on the target machine, not necessarily from the CUDA Toolkit installed on the CI runner.

For this reason, in CUDA-enabled DEB packaging, the project disables `CPACK_DEBIAN_PACKAGE_SHLIBDEPS`, preventing `dpkg-shlibdeps` from failing while trying to resolve `libcuda.so.1` on GitHub Actions.

## How to Check Whether CUDA Is Being Used

During transcription, run:

```bat
nvidia-smi -l 1
```

Look for the process:

```txt
obs64.exe
```

Then check whether VRAM/GPU usage increases during transcription.

The plugin also logs messages like:

```txt
[clip-cropper] Whisper CUDA compile flag CLIP_CROPPER_WHISPER_CUDA_REQUESTED=true
[clip-cropper] Whisper context params prepared. use_gpu=true
[clip-cropper] Whisper model loaded successfully. use_gpu=true cudaRequested=true
```

If the Whisper model is not found, the plugin disables the OpenAI model with:

```txt
openai_model = disabled
```

## Main Usage Flow

1. Record or select a video in OBS/plugin.
2. Open the editor/review screen.
3. Mark the desired range.
4. The plugin extracts audio using FFmpeg.
5. Whisper transcribes locally.
6. The transcript is saved to cache.
7. GPT generates curation prompt/context.
8. The review screen opens for validation.
9. The video/range is sent to Opus Clip.
10. Opus generates clips according to the curation settings.

## Troubleshooting

### The Plugin Cannot Find the Whisper Model

Check whether the `.bin` file uses one of the accepted exact names:

- `ggml-tiny.bin`
- `ggml-base.bin`
- `ggml-small.bin`
- `ggml-medium.bin`
- `ggml-large-v3.bin`

Also check whether it is inside:

```txt
data/obs-plugins/clip-cropper/models
```

In the local package:

```txt
package\data\obs-plugins\clip-cropper\models
```

In an installed OBS folder:

```txt
C:\Program Files\obs-studio\data\obs-plugins\clip-cropper\models
```

### OpenAI Appears as Disabled

This happens when the selected Whisper/GGML model was not found.

Copy the model into the `models` folder and reopen the settings.

### FFmpeg Was Not Found

Check whether this file exists:

```txt
data/obs-plugins/clip-cropper/ffmpeg/ffmpeg.exe
```

In the local package:

```txt
package\data\obs-plugins\clip-cropper\ffmpeg\ffmpeg.exe
```

It is also possible to manually configure the path with an environment variable:

```bat
set "CLIP_CROPPER_FFMPEG_PATH=C:\path\to\ffmpeg.exe"
```

### Linux Build Fails with `-fPIC` Error

Static libraries such as `libwhisper.a` and `libggml.a` must be compiled with position-independent code before being linked into the plugin `.so`.

The project forces this on Linux with:

```cmake
CMAKE_POSITION_INDEPENDENT_CODE ON
```

and flags such as:

```txt
-fPIC
-Xcompiler=-fPIC
```

### DEB Packaging Fails Looking for `libcuda.so.1`

`libcuda.so.1` comes from the NVIDIA driver on the target machine. It may not exist on CI runners.

For CUDA builds, the project disables automatic dependency generation through `dpkg-shlibdeps`.

## Quick Windows Commands

```bat
node .github\scripts\prepare-ffmpeg-runtime.mjs
set "CLIP_CROPPER_FFMPEG_RUNTIME_DIR=%CD%\.ffmpeg-runtime"
cmake --preset windows-ninja-cuda-x64 -DCLIP_CROPPER_FFMPEG_RUNTIME_DIR="%CLIP_CROPPER_FFMPEG_RUNTIME_DIR%"
cmake --build --preset windows-ninja-cuda-x64 --parallel
cmake --install build_cuda --prefix "%CD%\package"
```
