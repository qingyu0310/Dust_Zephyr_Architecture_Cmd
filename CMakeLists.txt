if(CONFIG_CMD_SHELL_VAR)
    target_include_directories(app PRIVATE shell)
    target_sources(app PRIVATE shell/shell.cpp)
endif()
