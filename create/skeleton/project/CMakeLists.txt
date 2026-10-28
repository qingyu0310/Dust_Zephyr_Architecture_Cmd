cmake_minimum_required(VERSION 3.20.0)

# ============================================================================
#
# 路径规划
#
# ============================================================================

# 当前项目（<用户区>/project）
set(PROJECT_ROOT 	"${CMAKE_CURRENT_SOURCE_DIR}/..")              # <用户区>/
# 子树（zephyr_user，架构层）
set(ZEPHYR_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr_user")
set(FW_ROOT         "${ZEPHYR_USER_DIR}/framework")
# 用户工作区内部目录（用户层，framework 六模块平铺用户区根）
set(APP_ALGORITHM   "${PROJECT_ROOT}/algorithm")                   # <用户区>/algorithm
set(APP_CMD         "${PROJECT_ROOT}/cmd")                         # <用户区>/cmd
set(APP_DRIVERS     "${PROJECT_ROOT}/drivers")                     # <用户区>/drivers
set(APP_INIT        "${PROJECT_ROOT}/init")                        # <用户区>/init
set(APP_MODULES     "${PROJECT_ROOT}/modules")                     # <用户区>/modules
set(APP_TOPIC       "${PROJECT_ROOT}/topic")                       # <用户区>/topic
set(APP_THREAD      "${CMAKE_CURRENT_SOURCE_DIR}/thread")          # <用户区>/project/thread/

# 业务门禁符号
set(CONFIG_SYM PRJ_MAIN)

# ============================================================================
#
# Zephyr 树自带
#
# ============================================================================

# 板级 overlay / conf / board.cmake
if(NOT DEFINED BOARD_CFG)
  	set(BOARD_CFG ${BOARD})
endif()

file(GLOB OVERLAY_FILES  ${CMAKE_CURRENT_SOURCE_DIR}/boards/*/${BOARD_CFG}/${BOARD}.overlay)
if(OVERLAY_FILES)
  	list(POP_FRONT OVERLAY_FILES DTC_OVERLAY_FILE)
endif()

file(GLOB PRJ_CONF_FILES ${CMAKE_CURRENT_SOURCE_DIR}/boards/*/${BOARD_CFG}/${BOARD}.conf)
if(PRJ_CONF_FILES)
  	list(POP_FRONT PRJ_CONF_FILES EXTRA_CONF_FILE)
endif()

file(GLOB BOARD_CMAKE   ${CMAKE_CURRENT_SOURCE_DIR}/boards/*/${BOARD_CFG}/board.cmake)
if(BOARD_CMAKE)
  	include(${BOARD_CMAKE})
endif()

# Zephyr SDK
if(DEFINED ENV{ZEPHYR_SDK_INSTALL_DIR})
  	set(ZEPHYR_SDK_INSTALL_DIR "$ENV{ZEPHYR_SDK_INSTALL_DIR}" CACHE PATH "" FORCE)
else()
  	set(ZEPHYR_SDK_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr-sdk-0.16.8" CACHE PATH "")
endif()

# ============================================================================
#
# 子树模块（zephyr_user：framework / cmsis / zephyr_user/drivers）
#
# ============================================================================

# SDK glue（HPM 平台可选）
if(DEFINED ENV{SDK_GLUE_DIR})
  	set(SDK_GLUE_DIR "$ENV{SDK_GLUE_DIR}")
else()
  	set(SDK_GLUE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../Zephyr_HPMicro/sdk_glue")
endif()
if(DEFINED ENV{SDK_GLUE_USER_DIR})
  	set(SDK_GLUE_USER_DIR "$ENV{SDK_GLUE_USER_DIR}")
else()
  	set(SDK_GLUE_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../Zephyr_HPMicro/sdk_glue_user")
endif()

# BOARD_ROOT / SOC_ROOT / DTS_ROOT 指向 zephyr_user
list(APPEND BOARD_ROOT "${ZEPHYR_USER_DIR}")
list(APPEND DTS_ROOT   "${ZEPHYR_USER_DIR}")
list(APPEND DTS_ROOT   "${ZEPHYR_USER_DIR}/dts")
if(EXISTS "${SDK_GLUE_DIR}")
  	# 用户层与官方并存：同名板卡在用户层加 u 前缀（如 uhpm6e00evk），两边都能构建
  	list(APPEND BOARD_ROOT "${SDK_GLUE_USER_DIR}")
  	list(APPEND SOC_ROOT   "${SDK_GLUE_USER_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_USER_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_USER_DIR}/dts")
  	list(APPEND BOARD_ROOT "${SDK_GLUE_DIR}")
  	list(APPEND SOC_ROOT   "${SDK_GLUE_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_DIR}/dts")
endif()

# 外来模块加载（module.yml 自动加载 Kconfig）
set(ZEPHYR_EXTRA_MODULES
  "${FW_ROOT}"
  "${ZEPHYR_USER_DIR}/platform/cmsis"
  "${ZEPHYR_USER_DIR}/drivers"
)
# sdk_glue 仅存在时加入——st 板卡无 HPM 树
if(EXISTS "${SDK_GLUE_DIR}")
  list(PREPEND ZEPHYR_EXTRA_MODULES "${SDK_GLUE_DIR}")
endif()
# 用户层 HPM 覆盖模块（只带 drivers，不重复 soc/boards/dts）
if(EXISTS "${SDK_GLUE_USER_DIR}/zephyr/module.yml")
  list(APPEND ZEPHYR_EXTRA_MODULES "${SDK_GLUE_USER_DIR}")
endif()

# 用户工作区模块（用户层六模块；目录空时跳过，填内容后自动加载）
foreach(_app IN ITEMS ${APP_ALGORITHM} ${APP_CMD} ${APP_DRIVERS} ${APP_INIT} ${APP_MODULES} ${APP_TOPIC})
  if(EXISTS "${_app}/CMakeLists.txt")
    list(APPEND ZEPHYR_EXTRA_MODULES "${_app}")
  endif()
endforeach()

# ============================================================================
#
# 接入 Zephyr 构建系统（必须在全部模块加载之后、任何 app 装配之前）
#
# ============================================================================

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_project)

# ============================================================================
#
# 装配子树架构（framework 各层 + zephyr_user/drivers）
#
# ============================================================================

if(CONFIG_${CONFIG_SYM})
  	add_subdirectory(${FW_ROOT}/drivers   ${CMAKE_CURRENT_BINARY_DIR}/framework/drivers)
  	add_subdirectory(${FW_ROOT}/algorithm ${CMAKE_CURRENT_BINARY_DIR}/framework/algorithm)
  	add_subdirectory(${FW_ROOT}/modules   ${CMAKE_CURRENT_BINARY_DIR}/framework/modules)
  	add_subdirectory(${FW_ROOT}/topic     ${CMAKE_CURRENT_BINARY_DIR}/framework/topic)
  	add_subdirectory(${FW_ROOT}/cmd       ${CMAKE_CURRENT_BINARY_DIR}/framework/cmd)
  	add_subdirectory(${FW_ROOT}/shell     ${CMAKE_CURRENT_BINARY_DIR}/framework/shell)
  	add_subdirectory(${FW_ROOT}/init      ${CMAKE_CURRENT_BINARY_DIR}/framework/init)

  	add_subdirectory(${ZEPHYR_USER_DIR}/drivers ${CMAKE_CURRENT_BINARY_DIR}/zephyr_user/drivers)
endif()

# 业务源码与系统入口（main() 由架构层 init 提供，业务项目不再有 src/）
target_include_directories(app PRIVATE
  	${FW_ROOT}/init
  	${FW_ROOT}/cmd
)

# 板卡专属全局 include（由 boards/<board>/board.cmake 的 BOARD_GLOBAL_INCLUDES 提供）
foreach(_dir IN LISTS BOARD_GLOBAL_INCLUDES)
  	zephyr_include_directories(${_dir})
endforeach()
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)

# 装配用户工作区（用户层六模块 + thread；目录空时跳过）
if(CONFIG_${CONFIG_SYM})
  	add_subdirectory(${APP_THREAD})
  	foreach(_app IN ITEMS ${APP_ALGORITHM} ${APP_CMD} ${APP_DRIVERS} ${APP_INIT} ${APP_MODULES} ${APP_TOPIC})
    	if(EXISTS "${_app}/CMakeLists.txt")
      		get_filename_component(_name "${_app}" NAME)
      		add_subdirectory(${_app} ${CMAKE_CURRENT_BINARY_DIR}/${_name})
    	endif()
  	endforeach()
endif()
