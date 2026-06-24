cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

set(OBS_PLUGIN_NAME "${_name}")

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(USE_FLATPAK_PATH "Use Flatpak OBS path" OFF)
option(ENABLE_WHISPER_CUDA "Enable Whisper CUDA backend" OFF)

option(CLIP_CROPPER_USE_LLAMA_CPP "Build the optional llama.cpp backend for local embeddings/reranking" OFF)
set(CLIP_CROPPER_LLAMA_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/llama.cpp" CACHE PATH "Path to the llama.cpp dependency")

option(CLIP_CROPPER_STATIC_DEPS "Build third-party dependencies added by this project as static libraries when possible" ON)
option(CLIP_CROPPER_STATIC_QT "Require and link a static Qt build instead of installing Qt runtime DLLs" OFF)
option(CLIP_CROPPER_DEPLOY_QT_RUNTIME "Install Qt runtime DLLs/plugins when Qt is linked dynamically" ON)
option(CLIP_CROPPER_STATIC_MSVC_RUNTIME "Use the static MSVC runtime (/MT) for targets created by this project" OFF)
option(CLIP_CROPPER_STATIC_CUDA_RUNTIME "Use the static CUDA runtime when CUDA support is enabled" ON)
option(CLIP_CROPPER_GGML_NATIVE "Build GGML with native CPU optimizations for the current build machine" OFF)

# Configure global dependency options before importing any third-party project.
# This is important because whisper.cpp and llama.cpp both read GGML_* cache
# variables while their targets are being created.
if (CLIP_CROPPER_STATIC_DEPS)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build third-party dependencies as static libraries" FORCE)
	set(CURL_USE_STATIC_LIBS ON CACHE BOOL "Prefer static libcurl when FindCURL is used in module mode" FORCE)
endif ()

if (MSVC AND CLIP_CROPPER_STATIC_MSVC_RUNTIME)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Use the static MSVC runtime" FORCE)
endif ()

if (ENABLE_WHISPER_CUDA AND CLIP_CROPPER_STATIC_CUDA_RUNTIME)
	set(CMAKE_CUDA_RUNTIME_LIBRARY Static CACHE STRING "Use the static CUDA runtime" FORCE)
endif ()

set(GGML_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(WHISPER_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(WHISPER_USE_SYSTEM_GGML OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE ${CLIP_CROPPER_GGML_NATIVE} CACHE BOOL "Build GGML with native CPU optimizations" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "Link GGML backends into the plugin instead of loading backend DLLs" FORCE)

if (ENABLE_WHISPER_CUDA)
	message(STATUS "Whisper/GGML CUDA backend requested")
	set(GGML_CUDA ON CACHE BOOL "Enable GGML CUDA backend" FORCE)

	if (NOT CMAKE_CUDA_ARCHITECTURES)
		# RTX 3070 Ti is Ampere / sm_86. Keep this deterministic because CI runners
		# usually cannot detect a native NVIDIA GPU architecture.
		set(CMAKE_CUDA_ARCHITECTURES 86 CACHE STRING "CUDA architectures for whisper.cpp/ggml" FORCE)
		message(STATUS "Defaulting CMAKE_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES}")
	endif ()

	if (MSVC)
		# CUDA 12.8 does not officially recognize Visual Studio 18 / MSVC 14.50 yet.
		set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler")
		set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcudafe --diag_suppress=177")
		set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcudafe --diag_suppress=221")
		set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcudafe --diag_suppress=550")
	endif ()
else ()
	message(STATUS "Whisper/GGML CUDA backend disabled")
	set(GGML_CUDA OFF CACHE BOOL "Enable GGML CUDA backend" FORCE)
endif ()

if (MSVC)
	foreach (flag_var
		CMAKE_CUDA_FLAGS
		CMAKE_CUDA_FLAGS_DEBUG
		CMAKE_CUDA_FLAGS_RELEASE
		CMAKE_CUDA_FLAGS_RELWITHDEBINFO
		CMAKE_CUDA_FLAGS_MINSIZEREL
	)
		string(REPLACE "/W3" "" ${flag_var} "${${flag_var}}")
		string(REPLACE "/utf-8" "" ${flag_var} "${${flag_var}}")
		string(REPLACE "/Brepro" "" ${flag_var} "${${flag_var}}")
		string(REPLACE "/permissive-" "" ${flag_var} "${${flag_var}}")
		string(REPLACE "/Gy" "" ${flag_var} "${${flag_var}}")
		string(REPLACE "/GL" "" ${flag_var} "${${flag_var}}")
		string(REPLACE "/Oi" "" ${flag_var} "${${flag_var}}")
	endforeach ()
endif ()

if (APPLE)
	set(GGML_METAL OFF CACHE BOOL "" FORCE)
	set(WHISPER_COREML OFF CACHE BOOL "" FORCE)
	set(GGML_ACCELERATE ON CACHE BOOL "" FORCE)
	set(CMAKE_OBJC_FLAGS "${CMAKE_OBJC_FLAGS} -fno-objc-arc")
	set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fno-objc-arc")
endif ()

if (UNIX AND NOT APPLE)
	# whisper.cpp/ggml are built as static libraries and linked into the OBS plugin
	# shared module. Linux linkers require those static objects to be built with
	# position-independent code, otherwise the final clip-cropper.so link fails
	# with relocation errors such as R_X86_64_PC32.
	set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Build static dependencies as PIC for Linux shared module linking" FORCE)
endif ()

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

if (WIN32)
	set(OBS_BUILD_DIR "${CMAKE_SOURCE_DIR}/.deps/obs-studio/build_x64")
elseif (APPLE)
	set(OBS_BUILD_DIR "${CMAKE_SOURCE_DIR}/.deps/obs-studio/build_universal")
endif ()

list(APPEND CMAKE_PREFIX_PATH
	"${OBS_BUILD_DIR}"
	"${OBS_BUILD_DIR}/libobs"
	"${OBS_BUILD_DIR}/cmake"
	"${CMAKE_SOURCE_DIR}/.deps"
)

message(STATUS "CMAKE_PREFIX_PATH = ${CMAKE_PREFIX_PATH}")
message(STATUS "OBS_BUILD_DIR = ${OBS_BUILD_DIR}")

file(GLOB_RECURSE LIBOBS_CONFIG
	"${CMAKE_SOURCE_DIR}/.deps/*libobsConfig.cmake"
	"${CMAKE_SOURCE_DIR}/.deps/*libobs-config.cmake"
)

if (APPLE)
	list(FILTER LIBOBS_CONFIG INCLUDE REGEX ".*/obs-studio-[^/]+/build_.*/libobs/libobsConfig\\.cmake$")
endif ()

message(STATUS "LIBOBS_CONFIG = ${LIBOBS_CONFIG}")

if (LIBOBS_CONFIG)
	list(GET LIBOBS_CONFIG 0 LIBOBS_CONFIG_FILE)
	get_filename_component(LIBOBS_DIR "${LIBOBS_CONFIG_FILE}" DIRECTORY)
	set(libobs_DIR "${LIBOBS_DIR}" CACHE PATH "Path to libobsConfig.cmake" FORCE)
	message(STATUS "libobs_DIR = ${libobs_DIR}")
endif ()

file(GLOB_RECURSE W32_PTHREADS_CONFIG
	"${CMAKE_SOURCE_DIR}/.deps/*w32-pthreadsConfig.cmake"
	"${CMAKE_SOURCE_DIR}/.deps/*w32-pthreads-config.cmake"
)

message(STATUS "W32_PTHREADS_CONFIG = ${W32_PTHREADS_CONFIG}")

if (WIN32)
	file(GLOB_RECURSE W32_PTHREADS_CONFIG
		"${CMAKE_SOURCE_DIR}/.deps/*w32-pthreadsConfig.cmake"
		"${CMAKE_SOURCE_DIR}/.deps/*w32-pthreads-config.cmake"
	)

	message(STATUS "W32_PTHREADS_CONFIG = ${W32_PTHREADS_CONFIG}")

	if (W32_PTHREADS_CONFIG)
		list(GET W32_PTHREADS_CONFIG 0 W32_PTHREADS_CONFIG_FILE)
		get_filename_component(W32_PTHREADS_DIR "${W32_PTHREADS_CONFIG_FILE}" DIRECTORY)

		set("w32-pthreads_DIR" "${W32_PTHREADS_DIR}" CACHE PATH "Path to w32-pthreadsConfig.cmake" FORCE)
		list(APPEND CMAKE_PREFIX_PATH "${W32_PTHREADS_DIR}")

		message(STATUS "w32-pthreads_DIR = ${w32-pthreads_DIR}")

		find_package(w32-pthreads REQUIRED CONFIG)
	else ()
		message(FATAL_ERROR "Could not find w32-pthreadsConfig.cmake under .deps")
	endif ()
endif ()

find_package(libobs REQUIRED)
find_package(CURL REQUIRED)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
	src/plugin-main.cpp

	src/ui/ui.cpp
	src/ui/ui-common.cpp
	src/ui/ui-common.hpp
	src/ui/settings-dialog.cpp
	src/ui/upload-flow.cpp
	src/ui/upload-flow.hpp
	src/ui/upload-confirm-dialog.cpp
	src/ui/curation-transcript-preparation.cpp
	src/ui/curation-transcript-preparation.hpp
	src/ui/review-scoring-preparation.cpp
	src/ui/review-scoring-preparation.hpp
	src/ui/video-editor-actions.cpp
	src/ui/advanced-settings-tree.cpp
	src/ui/advanced-settings-tree.hpp
	src/ui/video-marker-editor.cpp
	src/ui/video-marker-editor.hpp
	src/ui/upload-review-dialog.cpp
	src/ui/upload-review-dialog.hpp

	src/models/curation-settings.hpp
	src/models/transcript.hpp

	src/curation/feedback/curation-feedback-store.cpp
	src/curation/feedback/curation-feedback-store.hpp
	src/curation/feedback/boundary-calibration.cpp
	src/curation/feedback/boundary-calibration.hpp
	src/curation/scoring/text-analysis.cpp
	src/curation/scoring/text-analysis.hpp
	src/curation/scoring/transcript-index.cpp
	src/curation/scoring/transcript-index.hpp
	src/curation/scoring/word-boundary-snapper.cpp
	src/curation/scoring/word-boundary-snapper.hpp
	src/curation/scoring/clip-candidate.hpp
	src/curation/scoring/interval-dp-selector.cpp
	src/curation/scoring/interval-dp-selector.hpp
	src/curation/scoring/candidate-arc-planner.cpp
	src/curation/scoring/candidate-arc-planner.hpp
	src/curation/scoring/candidate-generator.cpp
	src/curation/scoring/candidate-generator.hpp
	src/curation/scoring/cheap-clip-scorer.cpp
	src/curation/scoring/cheap-clip-scorer.hpp
	src/curation/scoring/clip-ranker.cpp
	src/curation/scoring/clip-ranker.hpp
	src/curation/scoring/candidate-builder.cpp
	src/curation/scoring/candidate-builder.hpp
	src/curation/scoring/candidate-range-utils.cpp
	src/curation/scoring/candidate-range-utils.hpp
	src/curation/scoring/candidate-source-builder.cpp
	src/curation/scoring/candidate-source-builder.hpp
	src/curation/scoring/candidate-beam-expander.cpp
	src/curation/scoring/candidate-beam-expander.hpp
	src/curation/scoring/boundary-refinement-stage.cpp
	src/curation/scoring/boundary-refinement-stage.hpp
	src/curation/scoring/pipeline-progress.cpp
	src/curation/scoring/pipeline-progress.hpp
	src/curation/scoring/clip-scoring-pipeline-summary.cpp
	src/curation/scoring/clip-scoring-pipeline-summary.hpp
	src/curation/scoring/feedback-similarity-scorer.cpp
	src/curation/scoring/feedback-similarity-scorer.hpp
	src/curation/scoring/feedback-trained-ranker.cpp
	src/curation/scoring/feedback-trained-ranker.hpp
	src/curation/scoring/feedback-consistency-gate.cpp
	src/curation/scoring/feedback-consistency-gate.hpp
	src/curation/scoring/feedback-guided-candidate-generator.cpp
	src/curation/scoring/feedback-guided-candidate-generator.hpp
	src/curation/scoring/feedback-guided-candidate-stage.cpp
	src/curation/scoring/feedback-guided-candidate-stage.hpp
	src/curation/scoring/feedback-aware-interval-selector.cpp
	src/curation/scoring/feedback-aware-interval-selector.hpp
	src/curation/scoring/viewer-arc-gate.cpp
	src/curation/scoring/viewer-arc-gate.hpp
	src/curation/scoring/candidate-stage-limiter.cpp
	src/curation/scoring/candidate-stage-limiter.hpp
	src/curation/scoring/semantic-model.cpp
	src/curation/scoring/semantic-model.hpp
	src/curation/scoring/embedding-cache.cpp
	src/curation/scoring/embedding-cache.hpp
	src/curation/scoring/llama-cpp-model-resolver.cpp
	src/curation/scoring/llama-cpp-model-resolver.hpp
	src/curation/scoring/llama-cpp-embedding-provider.cpp
	src/curation/scoring/llama-cpp-embedding-provider.hpp
	src/curation/scoring/llama-cpp-reranker-provider.cpp
	src/curation/scoring/llama-cpp-reranker-provider.hpp
	src/curation/scoring/llama-server-embedding-provider.cpp
	src/curation/scoring/llama-server-embedding-provider.hpp
	src/curation/scoring/llama-server-reranker-provider.cpp
	src/curation/scoring/llama-server-reranker-provider.hpp
	src/curation/scoring/semantic-embedding-settings.cpp
	src/curation/scoring/semantic-embedding-settings.hpp
	src/curation/scoring/semantic-prototypes.cpp
	src/curation/scoring/semantic-prototypes.hpp
	src/curation/scoring/semantic-coarse-retriever.cpp
	src/curation/scoring/semantic-coarse-retriever.hpp
	src/curation/scoring/semantic-clip-scorer.cpp
	src/curation/scoring/semantic-clip-scorer.hpp
	src/curation/scoring/semantic-reranker.cpp
	src/curation/scoring/semantic-reranker.hpp
	src/curation/scoring/candidate-quality-gate.cpp
	src/curation/scoring/candidate-quality-gate.hpp
	src/curation/scoring/arc-dp-boundary-refiner.cpp
	src/curation/scoring/arc-dp-boundary-refiner.hpp
	src/curation/scoring/exchange-arc-boundary-refiner.cpp
	src/curation/scoring/exchange-arc-boundary-refiner.hpp
	src/curation/scoring/clip-scoring-pipeline.cpp
	src/curation/scoring/clip-scoring-pipeline.hpp
	src/curation/curation-preset.cpp
	src/curation/curation-preset-profile.cpp
	src/curation/curation-preset.hpp
	src/curation/curation-preset-profile.hpp
	src/curation/curation-signals.cpp
	src/curation/curation-signals.hpp

	src/transcription/transcript-store.cpp
	src/transcription/transcript-store.hpp
	src/transcription/whisperx-settings.cpp
	src/transcription/whisperx-settings.hpp
	src/transcription/whisperx-alignment-service.cpp
	src/transcription/whisperx-alignment-service.hpp
	src/transcription/realtime-transcription-service.cpp
	src/transcription/realtime-transcription-service.hpp


	src/utils/config.cpp
	src/utils/config.hpp
	src/utils/file.cpp
	src/utils/file.hpp

	src/opus/opus-api-types.hpp
	src/opus/opus-curation-payload-builder.cpp
	src/opus/opus-curation-payload-builder.hpp
	src/opus/opus-clip-client.cpp
	src/opus/opus-clip-client.hpp

	src/worker/upload-worker.cpp
	src/worker/upload-worker.hpp
)

target_include_directories(${CMAKE_PROJECT_NAME}
	PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/deps/whisper.cpp/include
	${CMAKE_CURRENT_SOURCE_DIR}/deps/whisper.cpp/ggml/include
)

target_link_libraries(${CMAKE_PROJECT_NAME}
	PRIVATE
	OBS::libobs
	CURL::libcurl
)

if (CLIP_CROPPER_USE_LLAMA_CPP)
	if (EXISTS "${CLIP_CROPPER_LLAMA_CPP_DIR}/CMakeLists.txt")
		message(STATUS "llama.cpp backend enabled at ${CLIP_CROPPER_LLAMA_CPP_DIR}")

		if (CLIP_CROPPER_STATIC_DEPS)
			set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
		endif ()

		set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
		set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
		set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
		set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
		set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
		set(LLAMA_USE_SYSTEM_GGML OFF CACHE BOOL "" FORCE)

		add_subdirectory("${CLIP_CROPPER_LLAMA_CPP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/deps/llama.cpp" EXCLUDE_FROM_ALL)

		target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CLIP_CROPPER_WITH_LLAMA_CPP=1)
		target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
			"${CLIP_CROPPER_LLAMA_CPP_DIR}/include"
			"${CLIP_CROPPER_LLAMA_CPP_DIR}/ggml/include"
		)

		if (TARGET llama)
			target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE llama)
		else ()
			message(FATAL_ERROR "llama.cpp was added, but the expected target 'llama' was not created")
		endif ()
	else ()
		message(FATAL_ERROR "CLIP_CROPPER_USE_LLAMA_CPP is ON, but deps/llama.cpp was not initialized")
	endif ()
else ()
	message(STATUS "llama.cpp native backend disabled. HTTP llama-server remains available; enable CLIP_CROPPER_USE_LLAMA_CPP for in-process embeddings/reranking.")
endif ()

if (ENABLE_FRONTEND_API)
	file(GLOB_RECURSE OBS_FRONTEND_API_CONFIG
		"${CMAKE_SOURCE_DIR}/.deps/*obs-frontend-apiConfig.cmake"
		"${CMAKE_SOURCE_DIR}/.deps/*obs-frontend-api-config.cmake"
	)

	if (APPLE)
		list(FILTER OBS_FRONTEND_API_CONFIG INCLUDE REGEX ".*/obs-studio-[^/]+/build_.*/UI/obs-frontend-api/obs-frontend-apiConfig\\.cmake$")
	endif ()

	message(STATUS "OBS_FRONTEND_API_CONFIG = ${OBS_FRONTEND_API_CONFIG}")

	if (OBS_FRONTEND_API_CONFIG)
		list(GET OBS_FRONTEND_API_CONFIG 0 OBS_FRONTEND_API_CONFIG_FILE)
		get_filename_component(OBS_FRONTEND_API_DIR "${OBS_FRONTEND_API_CONFIG_FILE}" DIRECTORY)
		set(obs-frontend-api_DIR "${OBS_FRONTEND_API_DIR}" CACHE PATH "Path to obs-frontend-apiConfig.cmake" FORCE)
		message(STATUS "obs-frontend-api_DIR = ${obs-frontend-api_DIR}")
	endif ()

	find_package(obs-frontend-api REQUIRED)

	target_link_libraries(${CMAKE_PROJECT_NAME}
		PRIVATE
		OBS::obs-frontend-api
	)
endif ()

message(STATUS "OBS_PLUGIN_NAME = ${OBS_PLUGIN_NAME}")
message(STATUS "TARGET = ${CMAKE_PROJECT_NAME}")

set(QT_RUNTIME_DLLS "")
set(QT_TLS_BACKENDS "")
set(QT_PLATFORM_PLUGINS "")
set(QT_MULTIMEDIA_PLUGINS "")
set(QT_IMAGEFORMAT_PLUGINS "")
set(QT_ICONENGINE_PLUGINS "")
set(QT_STYLE_PLUGINS "")

if (ENABLE_QT)
	find_package(Qt6 COMPONENTS
		Core
		Gui
		Widgets
		Network
		Multimedia
		MultimediaWidgets
		REQUIRED
	)

	target_link_libraries(${CMAKE_PROJECT_NAME}
		PRIVATE
		Qt6::Core
		Qt6::Gui
		Qt6::Widgets
		Qt6::Network
		Qt6::Multimedia
		Qt6::MultimediaWidgets
	)

	target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX)

	target_compile_options(${CMAKE_PROJECT_NAME}
		PRIVATE
		$<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
	)

	set_target_properties(${CMAKE_PROJECT_NAME}
		PROPERTIES
		AUTOMOC ON
		AUTOUIC ON
		AUTORCC ON
	)

	if (WIN32)
		get_target_property(QT_CORE_LOCATION Qt6::Core LOCATION)
		message(STATUS "Qt6::Core location = ${QT_CORE_LOCATION}")

		if (CLIP_CROPPER_STATIC_QT)
			if (QT_CORE_LOCATION MATCHES "\\.dll$")
				message(FATAL_ERROR
					"CLIP_CROPPER_STATIC_QT is ON, but Qt6::Core points to a DLL: ${QT_CORE_LOCATION}. "
					"Use a static Qt build and set CMAKE_PREFIX_PATH/Qt6_DIR to it, or disable CLIP_CROPPER_STATIC_QT."
				)
			endif ()

			if (COMMAND qt_import_plugins)
				# A static Qt build still needs its static plugin initialization stubs.
				# Qt imports a sane default set automatically; we only exclude SQL drivers
				# because this plugin does not use Qt SQL.
				qt_import_plugins(${CMAKE_PROJECT_NAME}
					EXCLUDE_BY_TYPE sqldrivers
				)
			else ()
				message(FATAL_ERROR "qt_import_plugins is unavailable; ensure Qt6::Core was found before enabling CLIP_CROPPER_STATIC_QT")
			endif ()
		else ()
			get_filename_component(QT_BIN_DIR "${QT_CORE_LOCATION}" DIRECTORY)
			get_filename_component(QT_ROOT_DIR "${QT_BIN_DIR}" DIRECTORY)

			set(QT_RUNTIME_DLLS
				"$<TARGET_FILE:Qt6::Core>"
				"$<TARGET_FILE:Qt6::Gui>"
				"$<TARGET_FILE:Qt6::Widgets>"
				"$<TARGET_FILE:Qt6::Network>"
				"$<TARGET_FILE:Qt6::Multimedia>"
				"$<TARGET_FILE:Qt6::MultimediaWidgets>"
			)

			file(GLOB QT_TLS_BACKENDS
				"${QT_ROOT_DIR}/plugins/tls/qschannelbackend.dll"
				"${QT_ROOT_DIR}/plugins/tls/qcertonlybackend.dll"
				"${QT_BIN_DIR}/../plugins/tls/qschannelbackend.dll"
				"${QT_BIN_DIR}/../plugins/tls/qcertonlybackend.dll"
			)

			file(GLOB QT_PLATFORM_PLUGINS
				"${QT_ROOT_DIR}/plugins/platforms/qwindows.dll"
				"${QT_BIN_DIR}/../plugins/platforms/qwindows.dll"
			)

			file(GLOB QT_MULTIMEDIA_PLUGINS
				"${QT_ROOT_DIR}/plugins/multimedia/*.dll"
				"${QT_BIN_DIR}/../plugins/multimedia/*.dll"
			)

			file(GLOB QT_IMAGEFORMAT_PLUGINS
				"${QT_ROOT_DIR}/plugins/imageformats/*.dll"
				"${QT_BIN_DIR}/../plugins/imageformats/*.dll"
			)

			file(GLOB QT_ICONENGINE_PLUGINS
				"${QT_ROOT_DIR}/plugins/iconengines/*.dll"
				"${QT_BIN_DIR}/../plugins/iconengines/*.dll"
			)

			file(GLOB QT_STYLE_PLUGINS
				"${QT_ROOT_DIR}/plugins/styles/*.dll"
				"${QT_BIN_DIR}/../plugins/styles/*.dll"
			)

			message(STATUS "QT_BIN_DIR = ${QT_BIN_DIR}")
			message(STATUS "QT_ROOT_DIR = ${QT_ROOT_DIR}")
			message(STATUS "QT_TLS_BACKENDS = ${QT_TLS_BACKENDS}")
			message(STATUS "QT_PLATFORM_PLUGINS = ${QT_PLATFORM_PLUGINS}")
			message(STATUS "QT_MULTIMEDIA_PLUGINS = ${QT_MULTIMEDIA_PLUGINS}")
			message(STATUS "QT_IMAGEFORMAT_PLUGINS = ${QT_IMAGEFORMAT_PLUGINS}")
			message(STATUS "QT_ICONENGINE_PLUGINS = ${QT_ICONENGINE_PLUGINS}")
			message(STATUS "QT_STYLE_PLUGINS = ${QT_STYLE_PLUGINS}")
		endif ()
	endif ()
endif ()


set(CLIP_CROPPER_FFMPEG_RUNTIME_DIR "" CACHE PATH "Directory containing the ffmpeg executable to bundle with the plugin")

if (NOT CLIP_CROPPER_FFMPEG_RUNTIME_DIR AND DEFINED ENV{CLIP_CROPPER_FFMPEG_RUNTIME_DIR})
	set(CLIP_CROPPER_FFMPEG_RUNTIME_DIR "$ENV{CLIP_CROPPER_FFMPEG_RUNTIME_DIR}")
endif ()

if (CLIP_CROPPER_FFMPEG_RUNTIME_DIR)
	cmake_path(CONVERT "${CLIP_CROPPER_FFMPEG_RUNTIME_DIR}" TO_CMAKE_PATH_LIST CLIP_CROPPER_FFMPEG_RUNTIME_DIR)
endif ()

set(CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE "")

if (CLIP_CROPPER_FFMPEG_RUNTIME_DIR)
	if (WIN32)
		set(CLIP_CROPPER_FFMPEG_EXECUTABLE_NAME "ffmpeg.exe")
	else ()
		set(CLIP_CROPPER_FFMPEG_EXECUTABLE_NAME "ffmpeg")
	endif ()

	set(CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE
		"${CLIP_CROPPER_FFMPEG_RUNTIME_DIR}/${CLIP_CROPPER_FFMPEG_EXECUTABLE_NAME}")

	if (EXISTS "${CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE}")
		message(STATUS "Bundled ffmpeg executable = ${CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE}")

		if (APPLE)
			target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE}")
			set_source_files_properties("${CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE}"
				PROPERTIES
				MACOSX_PACKAGE_LOCATION "Resources/ffmpeg"
			)
		endif ()
	else ()
		message(FATAL_ERROR
			"CLIP_CROPPER_FFMPEG_RUNTIME_DIR was set, but ${CLIP_CROPPER_FFMPEG_EXECUTABLE_NAME} was not found at ${CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE}")
	endif ()
endif ()

set_target_properties_plugin(${CMAKE_PROJECT_NAME}
	PROPERTIES
	OUTPUT_NAME ${_name}
)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)

# These were already configured near the top of the file, before llama.cpp and
# whisper.cpp are imported. Keep this section limited to Whisper-specific knobs
# so CUDA/GGML options cannot be applied too late.
target_compile_definitions(${CMAKE_PROJECT_NAME}
	PRIVATE
	CLIP_CROPPER_WHISPER_CUDA_REQUESTED=$<IF:$<BOOL:${ENABLE_WHISPER_CUDA}>,1,0>
)

option(CLANGD_LIGHT_MODE "Do not build heavy third-party deps for clangd" OFF)

if (CLANGD_LIGHT_MODE)
	target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/deps/whisper.cpp/include
		${CMAKE_CURRENT_SOURCE_DIR}/deps/whisper.cpp/ggml/include
	)
else()
	if (UNIX AND NOT APPLE)
		# Set this immediately before importing whisper.cpp as well because some
		# third-party CMake projects read CMAKE_POSITION_INDEPENDENT_CODE while their
		# targets are being created. The explicit -fPIC/-Xcompiler=-fPIC options below
		# are intentionally redundant: they make the CI link failure impossible even if
		# a dependency target ignores the target property.
		set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "Build static dependencies as PIC for Linux shared module linking" FORCE)
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
		add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fPIC>)
		if (ENABLE_WHISPER_CUDA)
			set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=-fPIC")
			add_compile_options($<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fPIC>)
		endif ()
	endif ()

	if (CLIP_CROPPER_STATIC_DEPS)
		set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	endif ()

	add_subdirectory(deps/whisper.cpp EXCLUDE_FROM_ALL)

	if (ENABLE_WHISPER_CUDA AND CLIP_CROPPER_STATIC_CUDA_RUNTIME)
		foreach (_clip_cropper_cuda_runtime_target
			whisper
			ggml
			ggml-base
			ggml-cpu
			ggml-cuda
		)
			if (TARGET ${_clip_cropper_cuda_runtime_target})
				set_target_properties(${_clip_cropper_cuda_runtime_target} PROPERTIES
					CUDA_RUNTIME_LIBRARY Static
				)
			endif ()
		endforeach ()
	endif ()

	if (UNIX AND NOT APPLE)
		foreach (_clip_cropper_pic_target
			whisper
			ggml
			ggml-base
			ggml-cpu
			ggml-cuda
		)
			if (TARGET ${_clip_cropper_pic_target})
				set_target_properties(${_clip_cropper_pic_target} PROPERTIES
					POSITION_INDEPENDENT_CODE ON
					INTERFACE_POSITION_INDEPENDENT_CODE ON
				)
				target_compile_options(${_clip_cropper_pic_target} PRIVATE
					$<$<COMPILE_LANGUAGE:C,CXX>:-fPIC>
					$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fPIC>
				)
			endif ()
		endforeach ()
	endif ()

	set_target_properties(whisper PROPERTIES
		COMPILE_WARNING_AS_ERROR OFF
	)

	if (MSVC)
		target_compile_options(whisper PRIVATE /WX-)
	endif()

	target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
		whisper
	)
endif()


if (WIN32)
	# Windows packages use the OBS plugin layout under obs-plugins/64bit.
	# Linux/macOS helpers already install the module to their platform-native OBS paths.
	install(TARGETS ${CMAKE_PROJECT_NAME}
		RUNTIME DESTINATION "obs-plugins/64bit"
		LIBRARY DESTINATION "obs-plugins/64bit"
		ARCHIVE DESTINATION "obs-plugins/64bit"
	)
endif ()

if (WIN32 AND ENABLE_QT AND CLIP_CROPPER_DEPLOY_QT_RUNTIME AND NOT CLIP_CROPPER_STATIC_QT)
	install(FILES ${QT_RUNTIME_DLLS}
		DESTINATION "obs-plugins/64bit"
	)

	if (QT_TLS_BACKENDS)
		install(FILES ${QT_TLS_BACKENDS}
			DESTINATION "obs-plugins/64bit/tls"
		)
	endif ()

	if (QT_PLATFORM_PLUGINS)
		install(FILES ${QT_PLATFORM_PLUGINS}
			DESTINATION "obs-plugins/64bit/platforms"
		)
	endif ()

	if (QT_MULTIMEDIA_PLUGINS)
		install(FILES ${QT_MULTIMEDIA_PLUGINS}
			DESTINATION "obs-plugins/64bit/multimedia"
		)
	endif ()

	if (QT_IMAGEFORMAT_PLUGINS)
		install(FILES ${QT_IMAGEFORMAT_PLUGINS}
			DESTINATION "obs-plugins/64bit/imageformats"
		)
	endif ()

	if (QT_ICONENGINE_PLUGINS)
		install(FILES ${QT_ICONENGINE_PLUGINS}
			DESTINATION "obs-plugins/64bit/iconengines"
		)
	endif ()

	if (QT_STYLE_PLUGINS)
		install(FILES ${QT_STYLE_PLUGINS}
			DESTINATION "obs-plugins/64bit/styles"
		)
	endif ()
endif ()

if (CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE)
	if (WIN32)
		install(PROGRAMS "${CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE}"
			DESTINATION "data/obs-plugins/${OBS_PLUGIN_NAME}/ffmpeg"
		)
	elseif (NOT APPLE)
		install(PROGRAMS "${CLIP_CROPPER_BUNDLED_FFMPEG_EXECUTABLE}"
			DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${OBS_PLUGIN_NAME}/ffmpeg"
		)
	endif ()
endif ()

if (WIN32)
	# Keep the Windows package aligned with OBS' data lookup:
	# package/data/obs-plugins/clip-cropper/...
	# Do not copy data/models here: local model binaries are large and must not be
	# included in install/package output. The models directory is created below with
	# only a .gitkeep placeholder.
	install(DIRECTORY data/
		DESTINATION "data/obs-plugins/${OBS_PLUGIN_NAME}"
		PATTERN "models" EXCLUDE
	)
	install(DIRECTORY tools/
		DESTINATION "data/obs-plugins/${OBS_PLUGIN_NAME}/tools"
	)
elseif (NOT APPLE)
	install(DIRECTORY data/
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${OBS_PLUGIN_NAME}"
		PATTERN "models" EXCLUDE
	)
	install(DIRECTORY tools/
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${OBS_PLUGIN_NAME}/tools"
	)
endif ()

# Do not package Whisper/GGML model binaries. They are large, optional, and should be
# provided by the user or release asset. Install only a placeholder so the expected
# models directory exists in fresh packages.
set(CLIP_CROPPER_MODELS_GITKEEP "${CMAKE_CURRENT_SOURCE_DIR}/data/models/.gitkeep")
if (NOT EXISTS "${CLIP_CROPPER_MODELS_GITKEEP}")
	set(CLIP_CROPPER_MODELS_GITKEEP "${CMAKE_CURRENT_SOURCE_DIR}/models/.gitkeep")
endif ()
if (NOT EXISTS "${CLIP_CROPPER_MODELS_GITKEEP}")
	set(CLIP_CROPPER_MODELS_GITKEEP "${CMAKE_CURRENT_BINARY_DIR}/models/.gitkeep")
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/models")
	file(WRITE "${CLIP_CROPPER_MODELS_GITKEEP}" "")
endif ()

if (WIN32)
	set(CLIP_CROPPER_MODELS_INSTALL_DEST "data/obs-plugins/${OBS_PLUGIN_NAME}/models")
elseif (APPLE)
	set(CLIP_CROPPER_MODELS_INSTALL_DEST "${OBS_PLUGIN_NAME}.plugin/Contents/Resources/models")
else ()
	set(CLIP_CROPPER_MODELS_INSTALL_DEST "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${OBS_PLUGIN_NAME}/models")
endif ()

install(FILES "${CLIP_CROPPER_MODELS_GITKEEP}"
	DESTINATION "${CLIP_CROPPER_MODELS_INSTALL_DEST}"
)
