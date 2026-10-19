# cmd 层：shell（连接底座 + 子命令分层）/ buzzer / flash / fatal

# shell 底座 + 子命令：log/var/thread 各自目录
target_include_directories(app PRIVATE
    shell
    shell/boot
    shell/log
    shell/var
    shell/thread
)

if(CONFIG_DUST_CMD_SHELL)
    target_sources(app PRIVATE
        shell/shell.cpp
        shell/boot/boot_cmd.cpp
        shell/thread/thread_cmd.cpp
    )
endif()
if(CONFIG_DUST_CMD_SHELL_VAR)
    target_sources(app PRIVATE shell/var/var.cpp)
endif()
if(CONFIG_DUST_CMD_SHELL_LOG)
    target_sources(app PRIVATE shell/log/log.cpp)
endif()

target_include_directories(app PRIVATE buzzer)
if(CONFIG_DUST_CMD_BUZZER)
    target_sources(app PRIVATE buzzer/buzzer.cpp)
endif()

target_include_directories(app PRIVATE flash)
if(CONFIG_DUST_CMD_W25Q128)
    target_sources(app PRIVATE flash/w25q128.cpp)
endif()

target_include_directories(app PRIVATE fatal)
target_sources(app PRIVATE fatal/fatal.cpp)
