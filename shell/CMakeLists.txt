if(CONFIG_SHELL_PID)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pid_cmd.cpp)
    target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(CONFIG_SHELL_DBG)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dbg_cmd.cpp)
    target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
