# 用户层 algorithm 装配 — 门禁 DUST_CHK_XXX
# 本文件由 project/CMakeLists.txt 的 add_subdirectory(${APP_ALGORITHM}) 引入
#
# 新增一个算法时照这块追加：
#
# if(CONFIG_DUST_CHK_XXX)
# 	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/check/xxx/xxx.cpp)
# 	target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/check/xxx)
# endif()
