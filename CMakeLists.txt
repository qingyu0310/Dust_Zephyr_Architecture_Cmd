target_include_directories(app PRIVATE shell)
if(CONFIG_DUST_CMD_SHELL_VAR)
    target_sources(app PRIVATE shell/shell.cpp)
endif()

target_include_directories(app PRIVATE buzzer)
if(CONFIG_DUST_CMD_BUZZER)
    target_sources(app PRIVATE buzzer/buzzer.cpp)
endif()

target_include_directories(app PRIVATE flash)
if(CONFIG_DUST_CMD_W25Q128)
    target_sources(app PRIVATE flash/w25q128.cpp)
endif()
