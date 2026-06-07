cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

set(OBS_PLUGIN_NAME "${_name}")

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(USE_FLATPAK_PATH "Use Flatpak OBS path" OFF)

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

if (W32_PTHREADS_CONFIG)
	list(GET W32_PTHREADS_CONFIG 0 W32_PTHREADS_CONFIG_FILE)
	get_filename_component(W32_PTHREADS_DIR "${W32_PTHREADS_CONFIG_FILE}" DIRECTORY)

	set("w32-pthreads_DIR" "${W32_PTHREADS_DIR}" CACHE PATH "Path to w32-pthreadsConfig.cmake" FORCE)

	list(APPEND CMAKE_PREFIX_PATH "${W32_PTHREADS_DIR}")

	message(STATUS "w32-pthreads_DIR = ${w32-pthreads_DIR}")
endif ()

find_package(libobs REQUIRED)
find_package(CURL REQUIRED)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
	src/plugin-main.cpp

	src/ui/ui.cpp
	src/ui/advanced-settings-tree.cpp
	src/ui/advanced-settings-tree.hpp
	src/ui/video-marker-editor.cpp
	src/ui/video-marker-editor.hpp
	src/ui/upload-review-dialog.cpp
	src/ui/upload-review-dialog.hpp

	src/models/curation-settings.hpp

	src/utils/config.cpp
	src/utils/config.hpp
	src/utils/file.cpp
	src/utils/file.hpp
	src/utils/request.cpp
	src/utils/request.hpp

	src/auth/google-oauth.cpp
	src/auth/google-oauth.hpp
	src/auth/oauth-callback-server.cpp
	src/auth/oauth-callback-server.hpp

	src/opus/opus-clip-client.cpp
	src/opus/opus-clip-client.hpp

	src/worker/upload-worker.cpp
	src/worker/upload-worker.hpp
)

target_include_directories(${CMAKE_PROJECT_NAME}
	PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
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

if (APPLE)
	set(OBS_PLUGIN_DIR "$ENV{HOME}/Library/Application Support/obs-studio/plugins/${OBS_PLUGIN_NAME}")
elseif (WIN32)
	set(OBS_PLUGIN_DIR "$ENV{APPDATA}/obs-studio/plugins/${OBS_PLUGIN_NAME}")
elseif (UNIX AND USE_FLATPAK_PATH)
	set(OBS_PLUGIN_DIR "$ENV{HOME}/.var/app/com.obsproject.Studio/config/obs-studio/plugins/${OBS_PLUGIN_NAME}")
elseif (UNIX)
	set(OBS_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/${OBS_PLUGIN_NAME}")
endif ()

set(OBS_PLUGIN_BIN_DIR "${OBS_PLUGIN_DIR}/bin/64bit")

message(STATUS "OBS_PLUGIN_NAME = ${OBS_PLUGIN_NAME}")
message(STATUS "OBS_PLUGIN_DIR = ${OBS_PLUGIN_DIR}")
message(STATUS "OBS_PLUGIN_BIN_DIR = ${OBS_PLUGIN_BIN_DIR}")
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

		add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_BIN_DIR}"

			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${QT_RUNTIME_DLLS}
			"${OBS_PLUGIN_BIN_DIR}/"
		)

		if (QT_TLS_BACKENDS)
			add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_BIN_DIR}/tls"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${QT_TLS_BACKENDS}
				"${OBS_PLUGIN_BIN_DIR}/tls/"
			)
		endif ()

		if (QT_PLATFORM_PLUGINS)
			add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_BIN_DIR}/platforms"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${QT_PLATFORM_PLUGINS}
				"${OBS_PLUGIN_BIN_DIR}/platforms/"
			)
		endif ()

		if (QT_MULTIMEDIA_PLUGINS)
			add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_BIN_DIR}/multimedia"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${QT_MULTIMEDIA_PLUGINS}
				"${OBS_PLUGIN_BIN_DIR}/multimedia/"
			)
		endif ()

		if (QT_IMAGEFORMAT_PLUGINS)
			add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_BIN_DIR}/imageformats"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${QT_IMAGEFORMAT_PLUGINS}
				"${OBS_PLUGIN_BIN_DIR}/imageformats/"
			)
		endif ()

		if (QT_ICONENGINE_PLUGINS)
			add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_BIN_DIR}/iconengines"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${QT_ICONENGINE_PLUGINS}
				"${OBS_PLUGIN_BIN_DIR}/iconengines/"
			)
		endif ()

		if (QT_STYLE_PLUGINS)
			add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_BIN_DIR}/styles"
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${QT_STYLE_PLUGINS}
				"${OBS_PLUGIN_BIN_DIR}/styles/"
			)
		endif ()
	endif ()
endif ()

set_target_properties_plugin(${CMAKE_PROJECT_NAME}
	PROPERTIES
	OUTPUT_NAME ${_name}
)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_BIN_DIR}"

	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	"$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
	"${OBS_PLUGIN_BIN_DIR}/"
)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
	"${CMAKE_CURRENT_SOURCE_DIR}/data"
	"${OBS_PLUGIN_DIR}/data"
)

install(TARGETS ${CMAKE_PROJECT_NAME}
	RUNTIME DESTINATION "obs-plugins/64bit"
	LIBRARY DESTINATION "obs-plugins/64bit"
	ARCHIVE DESTINATION "obs-plugins/64bit"
)

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

install(DIRECTORY data/locale/
	DESTINATION "data/obs-plugins/${OBS_PLUGIN_NAME}/locale"
	FILES_MATCHING PATTERN "*.ini"
)