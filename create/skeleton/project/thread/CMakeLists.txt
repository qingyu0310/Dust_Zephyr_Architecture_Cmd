# 业务线程装配 — 门禁 TRD_XXX 保持原名（不加 DUST_ 前缀）
#
# 新增一个线程时：
#   1. 在 project/Kconfig 加 config TRD_XXX
#   2. 在下面照这块追加
#
# if(CONFIG_TRD_XXX)
# 	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/xxx/trd_xxx.cpp)
# 	target_include_directories(app PRIVATE
# 		${CMAKE_CURRENT_SOURCE_DIR}
# 		${CMAKE_CURRENT_SOURCE_DIR}/xxx
# 	)
# endif()
