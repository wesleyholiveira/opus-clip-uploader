# CMake Windows compiler configuration module

include_guard(GLOBAL)

include(compiler_common)

set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)

message(DEBUG "Current Windows API version: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION_MAXIMUM)
  message(DEBUG "Maximum Windows API version: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION_MAXIMUM}")
endif()

set(_obs_windows_sdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

if(NOT _obs_windows_sdk_version)
  set(_obs_windows_sdk_version "${CMAKE_SYSTEM_VERSION}")
endif()

message(DEBUG "Effective Windows API version: ${_obs_windows_sdk_version}")

if(_obs_windows_sdk_version VERSION_LESS 10.0.20348)
  message(
    FATAL_ERROR
    "OBS requires Windows 10 SDK version 10.0.20348.0 or more recent.\n"
    "Please download and install the most recent Windows platform SDK."
  )
endif()

set(_obs_msvc_c_options /MP /Zc:__cplusplus /Zc:preprocessor)
set(_obs_msvc_cpp_options /MP /Zc:__cplusplus /Zc:preprocessor)

if(CMAKE_CXX_STANDARD GREATER_EQUAL 20)
  list(APPEND _obs_msvc_cpp_options /Zc:char8_t-)
endif()

add_compile_options(
	$<$<COMPILE_LANGUAGE:C,CXX>:/W3>
	$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
	$<$<COMPILE_LANGUAGE:C,CXX>:/Brepro>
	$<$<COMPILE_LANGUAGE:C,CXX>:/permissive->
	$<$<COMPILE_LANGUAGE:C,CXX>:/Gy>
	$<$<COMPILE_LANGUAGE:C,CXX>:/GL>
	$<$<COMPILE_LANGUAGE:C,CXX>:/Oi>
)

add_compile_definitions(
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
  _CRT_NONSTDC_NO_WARNINGS
  $<$<CONFIG:DEBUG>:DEBUG>
  $<$<CONFIG:DEBUG>:_DEBUG>
)

add_link_options(
  $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
  $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
  $<$<NOT:$<CONFIG:Debug>>:/LTCG>
  $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>
  /DEBUG
  /Brepro
)

if(CMAKE_COMPILE_WARNING_AS_ERROR)
  add_link_options(/WX)
endif()
