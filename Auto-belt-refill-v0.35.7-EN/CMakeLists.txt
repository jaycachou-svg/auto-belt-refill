cmake_minimum_required(VERSION 3.28)

project(AutoBeltRefill VERSION 0.35.7 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# Where things live. Change these if your layout differs.
# (Renamed from AUTOPOTION_* in v0.33 - same defaults, new variable names.)
# ---------------------------------------------------------------------------
set(AUTOBELTREFILL_SDK_DIR "D:/D2RPlugin/PluginSDK" CACHE PATH "Path to the D2RLoader PluginSDK checkout.")
set(AUTOBELTREFILL_GAME_DIR "D:/Diablo II Resurrected V3.2.92777" CACHE PATH "Diablo II: Resurrected install directory.")

# ---------------------------------------------------------------------------
# The SDK as a subproject, with its own samples and tests switched off.
# ---------------------------------------------------------------------------
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${AUTOBELTREFILL_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_sdk")

# ---------------------------------------------------------------------------
# The plugin. v0.33 renamed AutoPotion -> Auto Belt Refill; the DLL file name
# follows, and _deploy.bat deletes the legacy d2rl-autopotion.dll so the two
# never load at the same time.
# ---------------------------------------------------------------------------
add_library(AutoBeltRefill SHARED
	auto_belt_refill_plugin.cpp
	auto_belt_refill_plugin.rc
)

target_link_libraries(AutoBeltRefill PRIVATE D2RLPlugin::D2RLPlugin)

set_target_properties(AutoBeltRefill PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED TRUE
	CXX_EXTENSIONS FALSE
	OUTPUT_NAME "d2rl-auto-belt-refill"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/debug"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/release"
	LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/debug"
	LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/release"
	ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/lib/debug"
	ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib/release"
)

if(MSVC)
	# /utf-8: the default config template embedded in the plugin carries
	# Chinese comments; without the flag MSVC would re-encode the bytes.
	target_compile_options(AutoBeltRefill PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /utf-8>)
endif()

# ---------------------------------------------------------------------------
# cmake --build build --target deploy
# Copies the built DLL into the game's d2rloader\plugins\ folder. The game
# locks the DLL while running - close D2R before deploying.
# ---------------------------------------------------------------------------
add_custom_target(deploy
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:AutoBeltRefill>"
		"${AUTOBELTREFILL_GAME_DIR}/d2rloader/plugins/$<TARGET_FILE_NAME:AutoBeltRefill>"
	DEPENDS AutoBeltRefill
	COMMENT "Deploying $<TARGET_FILE_NAME:AutoBeltRefill>"
	VERBATIM
)
