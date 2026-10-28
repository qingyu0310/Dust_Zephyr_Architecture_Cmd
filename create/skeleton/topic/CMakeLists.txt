# 用户层 topic 装配 — 门禁 DUST_TPC_XXX
# 本文件由 project/CMakeLists.txt 的 add_subdirectory(${APP_TOPIC}) 引入
#
# 新增内容时照这块追加：
#
# if(CONFIG_DUST_TPC_XXX)
# 	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/xxx/xxx.cpp)
# 	target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/xxx)
# endif()
