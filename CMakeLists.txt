cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(USE_FLATPAK_PATH "Use Flatpak OBS path" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(CURL REQUIRED)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/plugin-main.cpp
  src/ui/ui.cpp
  src/utils/config.cpp
  src/utils/request.cpp
  src/utils/file.cpp
  src/worker/upload-worker.cpp
  src/worker/upload-worker.hpp
  src/auth/google-oauth.cpp
  src/auth/oauth-callback-server.cpp
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

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)

  target_link_libraries(${CMAKE_PROJECT_NAME}
    PRIVATE
      OBS::obs-frontend-api
  )
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Network REQUIRED)

  target_link_libraries(${CMAKE_PROJECT_NAME}
    PRIVATE
      Qt6::Core
      Qt6::Widgets
      Qt6::Network
  )

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
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME}
  PROPERTIES
    OUTPUT_NAME ${_name}
)

set(OBS_PLUGIN_NAME "${_name}")

if(APPLE)
  set(OBS_PLUGIN_DIR "$ENV{HOME}/Library/Application Support/obs-studio/plugins/${OBS_PLUGIN_NAME}")
elseif(WIN32)
  set(OBS_PLUGIN_DIR "$ENV{APPDATA}/obs-studio/plugins/${OBS_PLUGIN_NAME}")
elseif(UNIX AND USE_FLATPAK_PATH)
  set(OBS_PLUGIN_DIR "$ENV{HOME}/.var/app/com.obsproject.Studio/config/obs-studio/plugins/${OBS_PLUGIN_NAME}")
elseif(UNIX)
  set(OBS_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/${OBS_PLUGIN_NAME}")
endif()

message(STATUS "OBS_PLUGIN_NAME = ${OBS_PLUGIN_NAME}")
message(STATUS "OBS_PLUGIN_DIR = ${OBS_PLUGIN_DIR}")
message(STATUS "TARGET = ${CMAKE_PROJECT_NAME}")

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "${OBS_PLUGIN_DIR}/bin/64bit"

  COMMAND ${CMAKE_COMMAND} -E copy
    "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
    "${OBS_PLUGIN_DIR}/bin/64bit/"
)

install(TARGETS ${CMAKE_PROJECT_NAME}
  LIBRARY DESTINATION "plugins/${OBS_PLUGIN_NAME}/bin/64bit"
)