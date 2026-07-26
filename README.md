# cmd/ — 命令层

非业务性的辅助命令模块。存放与具体业务无关、但又需要运行时交互的工具。

## 目录

```
cmd/
├── README.md
├── shell/             ← 调试变量 shell
├── buzzer/            ← PWM 蜂鸣器
├── linker/            ← 链接段定义
├── build/             ← 编译脚本
├── CMakeLists.txt
└── Kconfig
```

## shell/ — 调试变量 shell

通过串口终端运行时修改参数值、查看状态，无需在业务代码中到处埋点。

### 思路

传统做法：每个模块自己写 `#ifdef DEBUG` + `printf`，调试代码散落在业务逻辑各处。

本方案：**注册宏 + 链接段 + 自维护 UART 线程**，把调试从横切关注点变成自包含模块。

```
业务代码          → REGISTER_SHELL_VAR("name", var)  一行
编译期            → 链接段 .shell_var 自动收集所有条目
运行时            → 一个 UART 线程统一处理 l/g/s 命令
```

### 链接段收集

```cpp
#define REGISTER_SHELL_VAR(name_, var_)                          \
    static ::debug::Entry s_shell_var_##var_                     \
        __attribute__((used, section(".shell_var"),              \
                       aligned(sizeof(void*)))) = {              \
        .name = (name_),                                         \
        .type = ::debug::TypeMap<decltype(var_)>::type,          \
        .ptr  = static_cast<void*>(&(var_)),                     \
    };
```

- 每个 `REGISTER_SHELL_VAR` 编译期在 `.shell_var` 段生成一个 Entry
- 链接器通过 `__shell_var_start` / `__shell_var_end` 提供边界
- 不需要中心化注册表，增删变量只改一行

### 类型自动推导

通过 `TypeMap<decltype(var_)>` 编译期匹配类型：

```cpp
template<> struct TypeMap<uint8_t>  { static constexpr auto type = VarType::Uint8;  };
template<> struct TypeMap<float>    { static constexpr auto type = VarType::Float;  };
template<> struct TypeMap<bool>     { static constexpr auto type = VarType::Bool;   };
```

支持全部基础类型：u8/i8/u16/i16/u32/i32/u64/i64/float/double/bool。

### 编译命令

编译时通过扫描文件夹检索设备树，在 `.overlay` 中指定 UART 设备用于 console + 调试复用：

```
zephyr,console  → 同一路串口，日志和调试共用
```

详见各板级文件夹下的 overlay。

### 为什么 cmd 持有一个自己的线程

项目约定业务线程统一放在 `project/thread/` 下。**cmd 是特例。**

理由：

1. **Zephyr shell 与 console 冲突** — Zephyr shell 后端劫持串口，与 `zephyr,console` 不兼容。要么 shell 独占一个串口，要么不用 shell。项目日志已经走 console 输出，加 shell 需要在同一路串口上跑两个协议。

2. **双串口不适合** — 如果 shell 独占一个串口，日志走 console 串口，用户需要两台终端、两条线，日志和调试分开看，每次操作要切换窗口。

3. **shell 不是业务逻辑** — 它不是传感器驱动、控制算法、通信协议，本质是一个**参数调试的辅助工具**。独立持有自己的 UART 线程是合理的特例。

### 实现

```cpp
// shell 线程循环 — DMA 空闲中断 sem 阻塞等待数据
k_sem_take(&uart_->sem_, K_FOREVER);
uart_->Read(buf, sizeof(buf));
// 逐字符处理命令
```

- 使用 `UartDma` 而非 `uart_poll_in`，DMA 回调写入 BipBuffer 后 `k_sem_give`
- `k_sem_take(K_FOREVER)` 阻塞等待，不轮询不占 CPU
- 使用 `LOG_INF` 输出，与主系统日志格式统一

### 命令

| 命令 | 功能 |
|------|------|
| `l` | 列出所有注册变量 |
| `g <name>` | 读取变量值 |
| `s <name> <value>` | 写入变量值 |
| `?` / `h` | 帮助 |

### 与 Zephyr shell 对比

| | 本项目方案 | Zephyr shell |
|---|-----------|-------------|
| 依赖 | 零（仅 `LOG_INF`） | 拉一整套 shell 框架 |
| UART 使用 | 复用现有 DMA 通道 | 独占串口 |
| 传输层 | UartDma + sem 阻塞 | uart_poll_in / fifo |
| 命令注册 | 一行宏 | SHELL_CMD_REGISTER + callback |
| 类型安全 | decltype 自动推导 | 字符串手动解析 |
| 链接段 | 编译期收集 | 运行时注册 |

## buzzer/ — PWM 蜂鸣器

蜂鸣器控制类，基于 PWM 发声，提供短路鸣、长鸣、紧急报警接口。

### 头文件常包含

`buzzer.hpp` 始终被 include（不依赖 CONFIG 开关），宏定义由 `#ifdef CONFIG_CMD_BUZZER` 控制：

```cpp
#ifdef CONFIG_CMD_BUZZER

class Buzzer { ... };
inline Buzzer& Instance();
#define EXEC_BUZZER_SHORT()    buzzer::Instance().Short()
#define EXEC_BUZZER_LONG()     buzzer::Instance().Long()
#define EXEC_BUZZER_ERR(...)   buzzer::Instance().Err(__VA_ARGS__)

#else

#define EXEC_BUZZER_SHORT()
#define EXEC_BUZZER_LONG()
#define EXEC_BUZZER_ERR(...)

#endif
```

**用意：** 调用方（如 Init_entry.cpp）无条件 `#include "buzzer.hpp"`，宏始终可展开。开启时执行实际动作，关闭时展开为空，不产生代码。

REGISTER_INIT 不放入头文件宏，直接写在 buzzer.cpp 里。buzzer.cpp 由 CONFIG_CMD_BUZZER 控制编译，不编译时 init 自然不存在。

### API

| 函数/宏 | 说明 |
|---------|------|
| `Init(spec)` | 初始化 PWM |
| `On()` / `Off()` | 持续发声 / 停止 |
| `Short()` | 短鸣两声（阻塞） |
| `Long()` | 长鸣两声（阻塞） |
| `Err(pattern)` | 紧急报警，循环 On() + pattern |
| `Beep(ms, count)` | 底层层单次发声 |
| `Instance()` | 全局实例 |
| `EXEC_BUZZER_SHORT/LONG/ERR` | 宏封装 |

### 应用

初始化阶段完成时调用 `EXEC_BUZZER_SHORT()` 提示。初始化失败 halt 时调用 `EXEC_BUZZER_ERR(nullptr)` 持续蜂鸣。

## linker/ — 链接段定义

`tflm_init.ld` 集中管理所有编译期收集段的边界符号。每个段使用 `SECTION_PROLOGUE` + `KEEP` 确保链接器不会优化掉。

| 段名 | 用途 | 遍历符号 |
|------|------|---------|
| `.user_init` | REGISTER_INIT 初始化项 | `__user_init_start/end` |
| `.can_rx1/2/3` | CAN_RX_HANDLER 接收分发 | `__can_rx1/2/3_start/end` |
| `.remote` | Remote 模块入口 | `__remote_start/end` |
| `.imu` | IMU 模块入口 | `__imu_start/end` |
| `.shell_var` | REGISTER_SHELL_VAR 调试变量 | `__shell_var_start/end` |

所有段均放入 `ROMABLE_REGION`，运行时通过 `__xxx_start/end` 符号边界遍历，不需要运行时注册表。

## build/ — 编译脚本

独立于 Zephyr `west build` 的封装，提供板级自动检索。

### build.bat

```
build <board_cfg> [west-args...]
```

- **默认板级**：`hpm6e00evk`
- **自动检索**：扫描 `projects/boards/*/<board_cfg>/` 下是否有 `.overlay`，找到后自动提取 board 目标名（如 `hpm5361icb`），并追加 `-- -DBOARD_CFG=<board_cfg>` 传给 CMake
- **fallback**：找不到 overlay 时直接按 `<board_cfg>` 作为 board 名编译

示例：
```
build hpm6e00evk              → 编译 hpm6e00evk 开发板
build board_rm_c              → 扫描找到 overlay → 编译 stm32f4_disco + BOARD_CFG
build hpm6e00evk -- -DCONFIG_TEST=y
```

### build.ps1

PowerShell 版，`-Name` 指定板级配置，`-Opts` 传额外参数。

## 设计原则

- **零入侵业务代码** — 业务方一行宏注册，不需要改框架
- **编译期收集** — 新增变量不需要中心化注册表
- **头文件常包含** — 宏始终可用，Kconfig 控制展开内容
- **编译单元控制 init** — REGISTER_INIT 写在 .cpp 里，由 Kconfig 编译条件决定是否注册
- **自包含** — 每个 cmd 独立持有自己的资源（UART、线程），不依赖 project/thread/ 的业务启动链
