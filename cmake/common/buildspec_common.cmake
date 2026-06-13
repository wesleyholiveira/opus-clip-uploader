# Common build dependencies module

include_guard(GLOBAL)

function(_normalize_windows_buildspec_vars)
	if (NOT OS_WINDOWS)
		return()
	endif ()

	set(arch "x64")
	set(platform "windows-x64")

	if (NOT CMAKE_SYSTEM_VERSION)
		if (DEFINED ENV{WindowsSDKVersion})
			set(_sdk_version "$ENV{WindowsSDKVersion}")
			string(REPLACE "\\" "" _sdk_version "${_sdk_version}")
			set(CMAKE_SYSTEM_VERSION "${_sdk_version}" CACHE STRING "Windows SDK version" FORCE)
		else ()
			set(CMAKE_SYSTEM_VERSION "10.0.26100.0" CACHE STRING "Windows SDK version" FORCE)
		endif ()
	endif ()

	if (NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
		set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "${CMAKE_SYSTEM_VERSION}" CACHE STRING "Windows SDK version" FORCE)
	endif ()

	set(arch "${arch}" CACHE STRING "OBS dependency architecture" FORCE)
	set(platform "${platform}" CACHE STRING "OBS dependency platform" FORCE)

	set(arch "${arch}" PARENT_SCOPE)
	set(platform "${platform}" PARENT_SCOPE)
	set(CMAKE_SYSTEM_VERSION "${CMAKE_SYSTEM_VERSION}" PARENT_SCOPE)
	set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}" PARENT_SCOPE)
endfunction()

function(_check_deps_version version)
	set(found FALSE)

	foreach (path IN LISTS CMAKE_PREFIX_PATH)
		if (EXISTS "${path}/share/obs-deps/VERSION")
			if (dependency STREQUAL qt6 AND NOT EXISTS "${path}/lib/cmake/Qt6/Qt6Config.cmake")
				set(found FALSE)
				continue ()
			endif ()

			file(READ "${path}/share/obs-deps/VERSION" _check_version)
			string(REPLACE "\n" "" _check_version "${_check_version}")
			string(REPLACE "-" "." _check_version "${_check_version}")
			string(REPLACE "-" "." version "${version}")

			if (_check_version VERSION_EQUAL version)
				set(found TRUE)
				break ()
			elseif (_check_version VERSION_LESS version)
				message(AUTHOR_WARNING "Older ${label} version detected in ${path}: \nFound ${_check_version}, require ${version}")
				list(REMOVE_ITEM CMAKE_PREFIX_PATH "${path}")
				list(APPEND CMAKE_PREFIX_PATH "${path}")
				set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH})
				continue ()
			else ()
				message(AUTHOR_WARNING "Newer ${label} version detected in ${path}: \nFound ${_check_version}, require ${version}")
				set(found TRUE)
				break ()
			endif ()
		endif ()
	endforeach ()

	return(PROPAGATE found CMAKE_PREFIX_PATH)
endfunction()

function(_setup_obs_studio)
	_normalize_windows_buildspec_vars()

	if (NOT libobs_DIR)
		set(_is_fresh --fresh)
	endif ()

	if (OS_WINDOWS)
		# Keep the plugin/main project on Ninja, but configure OBS itself
		# with Visual Studio x64. OBS 30.x CMake bootstrap can inject a
		# platform into Ninja sub-configures, causing:
		# "Ninja does not support platform specification, but platform Win32 was specified."
		set(_cmake_generator "Visual Studio 17 2022")
		set(_cmake_arch "-A" "x64")
		set(_obs_build_dir "build_${arch}_obs_vs")

		set(_cmake_extra
			"-Darch=${arch}"
			"-Dplatform=${platform}"
			"-DCMAKE_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION}"
			"-DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
			"-DCMAKE_SYSTEM_PROCESSOR=AMD64"
			"-DCMAKE_ENABLE_SCRIPTING=OFF"
		)

	elseif (OS_MACOS)
		set(_cmake_generator "Xcode")

		execute_process(
			COMMAND xcrun --sdk macosx --show-sdk-path
			OUTPUT_VARIABLE _macos_sdk_path
			OUTPUT_STRIP_TRAILING_WHITESPACE
			COMMAND_ERROR_IS_FATAL ANY
		)

		set(_cmake_arch "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64")
		set(_obs_build_dir "build_${arch}")

		set(_cmake_extra
			"-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
			"-DCMAKE_OSX_SYSROOT=${_macos_sdk_path}"
		)
	endif ()

	message(STATUS "Configure ${label} (${arch})")
	execute_process(
		COMMAND
			"${CMAKE_COMMAND}"
			-S "${dependencies_dir}/${_obs_destination}"
			-B "${dependencies_dir}/${_obs_destination}/${_obs_build_dir}"
			-G "${_cmake_generator}"
			${_cmake_arch}
			-DOBS_CMAKE_VERSION:STRING=3.0.0
			-DENABLE_PLUGINS:BOOL=OFF
			-DENABLE_PLUGIN_CONFIG:BOOL=OFF
			-DENABLE_WIN_CAPTURE:BOOL=OFF
			-DENABLE_VIRTUALCAM:BOOL=OFF
			-DENABLE_GRAPHICS_HOOK:BOOL=OFF
			-DENABLE_FRONTEND:BOOL=OFF
			-DOBS_VERSION_OVERRIDE:STRING=${_obs_version}
			"-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
			${_is_fresh}
			${_cmake_extra}
		RESULT_VARIABLE _process_result
		COMMAND_ERROR_IS_FATAL ANY
	)
	message(STATUS "Configure ${label} (${arch}) - done")

	message(STATUS "Build ${label} (Debug - ${arch})")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" --build "${_obs_build_dir}" --target obs-frontend-api --config Debug --parallel
		WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
		RESULT_VARIABLE _process_result
		COMMAND_ERROR_IS_FATAL ANY
	)
	message(STATUS "Build ${label} (Debug - ${arch}) - done")

	message(STATUS "Build ${label} (Release - ${arch})")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" --build "${_obs_build_dir}" --target obs-frontend-api --config Release --parallel
		WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
		RESULT_VARIABLE _process_result
		COMMAND_ERROR_IS_FATAL ANY
	)
	message(STATUS "Build ${label} (Release - ${arch}) - done")

	message(STATUS "Install ${label} (${arch})")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" --install "${_obs_build_dir}" --component Development --config Debug --prefix "${dependencies_dir}"
		WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
		RESULT_VARIABLE _process_result
		COMMAND_ERROR_IS_FATAL ANY
	)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" --install "${_obs_build_dir}" --component Development --config Release --prefix "${dependencies_dir}"
		WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
		RESULT_VARIABLE _process_result
		COMMAND_ERROR_IS_FATAL ANY
	)
	message(STATUS "Install ${label} (${arch}) - done")
endfunction()

function(_check_dependencies)
	_normalize_windows_buildspec_vars()

	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)

	string(JSON dependency_data GET ${buildspec} dependencies)

	foreach (dependency IN LISTS dependencies_list)
		message(STATUS "Resolved OBS dependency arch = ${arch}")
		message(STATUS "Resolved OBS dependency platform = ${platform}")

		string(JSON data GET ${dependency_data} ${dependency})
		string(JSON version GET ${data} version)
		string(JSON hash GET ${data} hashes ${platform})
		string(JSON url GET ${data} baseUrl)
		string(JSON label GET ${data} label)
		string(JSON revision ERROR_VARIABLE error GET ${data} revision ${platform})

		message(STATUS "Setting up ${label} (${arch})")

		set(file "${${dependency}_filename}")
		set(destination "${${dependency}_destination}")
		string(REPLACE "VERSION" "${version}" file "${file}")
		string(REPLACE "VERSION" "${version}" destination "${destination}")
		string(REPLACE "ARCH" "${arch}" file "${file}")
		string(REPLACE "ARCH" "${arch}" destination "${destination}")

		if (revision)
			string(REPLACE "_REVISION" "_v${revision}" file "${file}")
			string(REPLACE "-REVISION" "-v${revision}" file "${file}")
		else ()
			string(REPLACE "_REVISION" "" file "${file}")
			string(REPLACE "-REVISION" "" file "${file}")
		endif ()

		if (EXISTS "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256")
			file(READ "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256" OBS_DEPENDENCY_${dependency}_${arch}_HASH)
		endif ()

		set(skip FALSE)
		if (dependency STREQUAL prebuilt OR dependency STREQUAL qt6)
			if (OBS_DEPENDENCY_${dependency}_${arch}_HASH STREQUAL ${hash})
				_check_deps_version(${version})
				if (found)
					set(skip TRUE)
				endif ()
			endif ()
		endif ()

		if (skip)
			message(STATUS "Setting up ${label} (${arch}) - skipped")
			continue ()
		endif ()

		if (dependency STREQUAL obs-studio)
			set(url ${url}/${file})
		else ()
			set(url ${url}/${version}/${file})
		endif ()

		if (NOT EXISTS "${dependencies_dir}/${file}")
			message(STATUS "Downloading ${url}")
			file(DOWNLOAD "${url}" "${dependencies_dir}/${file}" STATUS download_status EXPECTED_HASH SHA256=${hash})

			list(GET download_status 0 error_code)
			list(GET download_status 1 error_message)

			if (error_code GREATER 0)
				message(STATUS "Downloading ${url} - Failure")
				message(FATAL_ERROR "Unable to download ${url}, failed with error: ${error_message}")
				file(REMOVE "${dependencies_dir}/${file}")
			else ()
				message(STATUS "Downloading ${url} - done")
			endif ()
		endif ()

		if (NOT OBS_DEPENDENCY_${dependency}_${arch}_HASH STREQUAL ${hash})
			file(REMOVE_RECURSE "${dependencies_dir}/${destination}")
		endif ()

		if (NOT EXISTS "${dependencies_dir}/${destination}")
			file(MAKE_DIRECTORY "${dependencies_dir}/${destination}")

			if (dependency STREQUAL obs-studio)
				file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}")
			else ()
				file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}/${destination}")
			endif ()
		endif ()

		file(WRITE "${dependencies_dir}/.dependency_${dependency}_${arch}.sha256" "${hash}")

		if (dependency STREQUAL prebuilt)
			list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}/${destination}")
		elseif (dependency STREQUAL qt6)
			list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}/${destination}")
		elseif (dependency STREQUAL obs-studio)
			set(_obs_version ${version})
			set(_obs_destination "${destination}")
			list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}")
		endif ()

		message(STATUS "Setting up ${label} (${arch}) - done")
	endforeach ()

	list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
	set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE PATH "CMake prefix search path" FORCE)

	_setup_obs_studio()
endfunction()