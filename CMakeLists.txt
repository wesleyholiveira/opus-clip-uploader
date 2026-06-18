cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

set(OBS_PLUGIN_NAME "${_name}")

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(USE_FLATPAK_PATH "Use Flatpak OBS path" OFF)
option(ENABLE_WHISPER_CUDA "Enable Whisper CUDA backend" OFF)

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
	src/ui/gpt-review-prompt.cpp
	src/ui/gpt-review-prompt.hpp
	src/ui/video-editor-actions.cpp
	src/ui/advanced-settings-tree.cpp
	src/ui/advanced-settings-tree.hpp
	src/ui/video-marker-editor.cpp
	src/ui/video-marker-editor.hpp
	src/ui/upload-review-dialog.cpp
	src/ui/upload-review-dialog.hpp

	src/models/curation-settings.hpp
	src/models/transcript.hpp

	src/curation/domain/curation-plan.hpp
	src/curation/analysis/named-reference-detector.cpp
	src/curation/analysis/named-reference-detector.hpp
	src/curation/compiler/opus-prompt-compiler.cpp
	src/curation/compiler/opus-prompt-compiler.hpp
	src/curation/compiler/prompt-quality-gate.cpp
	src/curation/compiler/prompt-quality-gate.hpp
	src/curation/service/curation-service.cpp
	src/curation/service/curation-service.hpp
	src/curation/curation-preset.cpp
	src/curation/curation-preset.hpp
	src/curation/curation-signals.cpp
	src/curation/curation-signals.hpp
	src/curation/curation-intent.cpp
	src/curation/curation-intent.hpp
	src/curation/curation-rules.cpp
	src/curation/curation-rules.hpp
	src/curation/opus-prompt-renderer.cpp
	src/curation/opus-prompt-renderer.hpp

	src/transcription/transcript-store.cpp
	src/transcription/transcript-store.hpp
	src/transcription/realtime-transcription-service.cpp
	src/transcription/realtime-transcription-service.hpp

	src/gpt/openai-chat-payload.cpp
	src/gpt/openai-chat-payload.hpp
	src/gpt/gpt-prompt-client.cpp
	src/gpt/gpt-prompt-client.hpp
	src/gpt/gpt-prompt-store.cpp
	src/gpt/gpt-prompt-store.hpp

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
set(WHISPER_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GGML_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE ON CACHE BOOL "" FORCE)

target_compile_definitions(${CMAKE_PROJECT_NAME}
	PRIVATE
	CLIP_CROPPER_WHISPER_CUDA_REQUESTED=$<IF:$<BOOL:${ENABLE_WHISPER_CUDA}>,1,0>
)

if (ENABLE_WHISPER_CUDA)
	message(STATUS "Whisper CUDA/GPU backend requested")
	set(GGML_CUDA ON CACHE BOOL "" FORCE)

	if (NOT CMAKE_CUDA_ARCHITECTURES)
		# GitHub-hosted Ubuntu runners do not expose a real NVIDIA GPU, so CUDA's
		# `native` architecture detection falls back with noisy nvcc warnings. Use
		# a deterministic Ampere target by default; callers can still override this
		# through -DCMAKE_CUDA_ARCHITECTURES=... when needed.
		set(CMAKE_CUDA_ARCHITECTURES 86 CACHE STRING "CUDA architectures for whisper.cpp/ggml" FORCE)
		message(STATUS "Defaulting CMAKE_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES}")
	endif ()

	if (MSVC)
		# CUDA 12.8 does not officially recognize Visual Studio 18 / MSVC 14.50 yet.
		# Keep this at the root level so local builds and CI use the same reproducible flags
		# without editing deps/whisper.cpp.
		set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler")

		# ggml-cuda currently appends `-Werror all-warnings`, so these NVCC diagnostics
		# become hard errors with newer CUDA/MSVC combinations.
		set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcudafe --diag_suppress=221")
		set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcudafe --diag_suppress=177")
	endif ()
else ()
	message(STATUS "Whisper CUDA/GPU backend disabled")
	set(GGML_CUDA OFF CACHE BOOL "" FORCE)
endif ()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

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

set(GGML_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(WHISPER_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

if (MSVC AND ENABLE_WHISPER_CUDA)
	set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler -Xcudafe --diag_suppress=177 -Xcudafe --diag_suppress=221 -Xcudafe --diag_suppress=550")
endif ()

if (APPLE)
	set(GGML_METAL OFF CACHE BOOL "" FORCE)
	set(WHISPER_COREML OFF CACHE BOOL "" FORCE)
	set(GGML_ACCELERATE ON CACHE BOOL "" FORCE)
	set(CMAKE_OBJC_FLAGS "${CMAKE_OBJC_FLAGS} -fno-objc-arc")
	set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fno-objc-arc")
endif ()

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

	add_subdirectory(deps/whisper.cpp EXCLUDE_FROM_ALL)

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

if (WIN32 AND ENABLE_QT)
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
elseif (NOT APPLE)
	install(DIRECTORY data/
		DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${OBS_PLUGIN_NAME}"
		PATTERN "models" EXCLUDE
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
