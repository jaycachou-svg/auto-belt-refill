cmake_minimum_required(VERSION 3.28)

project(AutoBeltRefill VERSION 0.35.7 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# 路径设置。若你的目录布局不同，改这里即可。
# （v0.33 由 AUTOPOTION_* 改名为 AUTOBELTREFILL_*：默认值不变，仅变量名更新。）
# ---------------------------------------------------------------------------
set(AUTOBELTREFILL_SDK_DIR "D:/D2RPlugin/PluginSDK" CACHE PATH "D2RLoader PluginSDK 源码目录。")
set(AUTOBELTREFILL_GAME_DIR "D:/Diablo II Resurrected V3.2.92777" CACHE PATH "暗黑破坏神2：重制版 安装目录。")

# ---------------------------------------------------------------------------
# 以子项目方式引入 SDK，并关闭其自带的示例与测试。
# ---------------------------------------------------------------------------
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${AUTOBELTREFILL_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_sdk")

# ---------------------------------------------------------------------------
# 插件本体。v0.33 起 AutoPotion 改名 Auto Belt Refill，DLL 文件名随之更新。
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
	# /utf-8：插件内嵌的默认配置模板带中文注释；不加该参数 MSVC 会
	# 重新编码字节、导致模板损坏。
	target_compile_options(AutoBeltRefill PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /utf-8>)
endif()

# ---------------------------------------------------------------------------
# cmake --build build --target deploy
# 把编译好的 DLL 复制到游戏目录 d2rloader\plugins\。
# 注意：游戏正在运行时会占用 DLL 导致复制失败——先关游戏再执行。
# ---------------------------------------------------------------------------
add_custom_target(deploy
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:AutoBeltRefill>"
		"${AUTOBELTREFILL_GAME_DIR}/d2rloader/plugins/$<TARGET_FILE_NAME:AutoBeltRefill>"
	DEPENDS AutoBeltRefill
	COMMENT "Deploying $<TARGET_FILE_NAME:AutoBeltRefill>"
	VERBATIM
)
