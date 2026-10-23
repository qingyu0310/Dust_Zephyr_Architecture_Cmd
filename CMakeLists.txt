# cmd 层：buzzer / flash / fatal / linker

target_include_directories(app PRIVATE buzzer)
if(CONFIG_DUST_CMD_BUZZER)
    target_sources(app PRIVATE buzzer/buzzer.cpp)
endif()

target_include_directories(app PRIVATE flash)
if(CONFIG_DUST_CMD_W25Q128)
    target_sources(app PRIVATE flash/w25q128.cpp)
endif()

target_include_directories(app PRIVATE fatal)
# init/thread 的 include 由 framework/init/CMakeLists.txt 全局加入（thread_registry.hpp）
target_sources(app PRIVATE fatal/fatal.cpp)
