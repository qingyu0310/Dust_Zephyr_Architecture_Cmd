if(CONFIG_CMD_SHELL_VAR)
    target_include_directories(app PRIVATE shell)
    target_sources(app PRIVATE shell/shell.cpp)
endif()

target_include_directories(app PRIVATE buzzer)
if(CONFIG_CMD_BUZZER)
    target_sources(app PRIVATE buzzer/buzzer.cpp)
endif()
