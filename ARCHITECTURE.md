# cmd/ 架构说明

`cmd/` 是当前工程里的运行期辅助执行层。

它不属于传统意义上的业务线程，也不完全等同于设备驱动。它负责承接一些横切型能力：

- 调试变量的运行时查看和修改；
- 初始化阶段的声音提示；
- 关键参数的 Flash 持久化；
- 链接段注册表的集中定义；
- 构建入口脚本。

这些能力有一个共同特点：很多上层代码都想随手调用，但它们又不应该把业务模块、项目线程和底层驱动绑死。

所以 `cmd/` 的核心设计目标不是“多做一层命令封装”，而是：

```text
调用方可以长期包含稳定头文件
    -> Kconfig 决定功能是否真实存在
        -> CMake 决定实现文件是否进入编译
            -> 宏在关闭功能时提供零侵入降级
```

这也是为什么当前 `shell`、`buzzer`、`flash` 都采用了宏入口。

---

## 当前目录

```text
cmd/
├── ARCHITECTURE.md
├── README.md
├── CMakeLists.txt
├── Kconfig
├── build/
│   ├── build.bat
│   └── build.ps1
├── linker/
│   └── tflm_init.ld
├── shell/
│   ├── shell.hpp
│   └── shell.cpp
├── buzzer/
│   ├── buzzer.hpp
│   └── buzzer.cpp
└── flash/
    ├── partition.hpp
    ├── w25q128.hpp
    └── w25q128.cpp
```

当前 `cmd/` 里有三类运行期组件：

| 组件 | 能力 | 编译开关 | 底层依赖 |
| --- | --- | --- | --- |
| `shell/` | UART 调试变量控制台 | `CMD_SHELL_VAR` | `COM_UART_DMA` |
| `buzzer/` | PWM 蜂鸣器提示 | `CMD_BUZZER` | `DEV_PWM` |
| `flash/` | W25Q128 SPI NOR Flash | `CMD_W25Q128` / `CMD_FLASH` | `COM_SPI` |

`linker/` 和 `build/` 不直接提供业务能力：

- `linker/` 提供链接段边界，支撑注册宏；
- `build/` 提供本地构建脚本，不进入固件。

---

## cmd/ 的定位

### 它管什么

`cmd/` 管的是“辅助执行入口”：

- 可以被多个模块调用；
- 不属于某一个机器人业务；
- 关闭后不应该导致调用方大面积加 `#ifdef`；
- 启用后可以拥有自己的初始化、资源和线程；
- 通常服务调试、提示、持久化、实验和启动辅助。

例如：

```text
Init_entry.cpp
    -> EXEC_BUZZER_SHORT()
    -> EXEC_BUZZER_ERR(nullptr)

imu_device_layer.hpp
    -> EXEC_FLASH_WRITE(...)

icm42688p.cpp / bmi088.cpp
    -> EXEC_FLASH_READ(...)

业务测试代码
    -> REGISTER_SHELL_VAR("name", variable)
```

这些调用点来自不同层，但调用的都是“辅助执行能力”。如果把这些能力放回具体模块里，会让上层到处携带条件编译和硬件细节。

### 它不管什么

`cmd/` 不应该成为第二个 `project/`。

它不应该负责：

- 底盘、云台、遥控器、IMU 的业务状态机；
- 具体机器人策略；
- 控制闭环计算；
- topic 消息定义；
- 高实时性的热路径调度；
- 某个项目专属的参数表和业务规则。

更准确地说，`cmd/` 可以被业务调用，但不应该反向理解业务。

---

## 为什么 shell 允许独立维护线程

旧版 `cmd/ARCHITECTURE.md` 写过“cmd 不定义线程”。这条规则在当前架构里已经不适用。

现在的 `shell/` 明确允许维护自己的线程，原因不是破坏项目层边界，而是它本质上不是业务线程。

### shell 是调试控制面，不是机器人业务线程

`project/thread/` 里的线程主要表达机器人运行能力：

```text
remote thread
imu thread
can tx thread
chassis thread
gimbal thread
pc thread
test thread
```

这些线程会参与系统业务数据流。它们通常会读写 topic、控制设备、执行算法，或者把数据发送到外部。

而 `cmd/shell` 做的是：

```text
串口输入
    -> 解析 l/g/s/?
        -> 遍历 .shell_var
            -> 读写被注册的调试变量
                -> 通过 LOG_INF 输出结果
```

它是一个调试控制面，不是某个功能模块的业务循环。

如果强行把 shell 线程放进 `project/thread/`，会出现一个尴尬结果：每个项目都要知道“调试控制台如何运行”，每个项目都要替它维护线程入口。这样反而提高了耦合度。

把线程放在 `cmd/shell` 内部后，调用方只需要关心：

```cpp
REGISTER_SHELL_VAR("gain", gain);
```

至于它如何接 UART、如何收行、如何解析命令、如何遍历变量，全部由 `cmd/shell` 自己负责。

### shell 自带线程能减少项目层噪声

当前 `DbgConsole` 内部持有：

```cpp
Thread<2048> thread_;
UartDma     *uart_;
uint8_t      line_buf_[128];
```

它需要一个长期阻塞等待输入的循环：

```text
k_sem_take(&uart_->sem_, K_FOREVER)
    -> uart_->Read(...)
        -> ProcessLine(...)
```

这个循环和 IMU、底盘、云台不在同一个抽象层上。

它不生产机器人控制数据，也不应该通过 topic 参与实时业务链。它只在用户敲命令时被唤醒，平时阻塞在信号量上。

所以让它独立维护线程，反而更符合低耦合目标：

```text
业务线程不认识 shell 线程
shell 线程不认识具体业务线程
双方只通过 .shell_var 中的调试变量发生关系
```

### shell 仍然服从统一启动系统

独立维护线程不等于绕开系统启动。

当前 `shell.cpp` 仍然使用 `REGISTER_INIT()` 接入 `.user_init`：

```cpp
REGISTER_INIT(thread_init,  PreInit,    High, "dbg_init");
REGISTER_INIT(thread_start, LateThread, Low,  "dbg_start");
```

也就是说：

- UART DMA 初始化发生在 `PreInit`；
- shell 线程启动发生在 `LateThread`；
- 初始化失败策略由 `InitLevel` 表达；
- 启动顺序仍由 `project/apps/Init_entry.cpp` 统一遍历。

这保持了两个边界：

```text
资源与线程由 shell 自己持有
启动时机由统一启动器管理
```

这比把 shell 写成 project thread 更干净，因为它把“调试控制面怎么运行”从项目业务里抽走了。

### 为什么不用 Zephyr shell

当前 `shell/` 不是 Zephyr 原生 shell 的简单包装。

它是一个轻量调试变量控制台，原因有三点。

第一，项目已经把日志放在 `zephyr,console` 上。Zephyr shell 通常需要自己的 shell 后端配置，容易和 console/log 使用方式产生冲突，或者要求额外串口。

第二，调试变量需求很简单：

```text
l              列表
g <name>       读取
s <name> <v>   修改
? / h          帮助
```

为了这几个命令拉起完整 shell 框架，反而会让依赖变重。

第三，当前工程已经有自己的 UART DMA 抽象。`cmd/shell` 直接复用 `UartDma` 的 DMA 接收和信号量唤醒，避免轮询，也避免额外终端。

因此这里采用：

```text
UartDma + Thread + .shell_var + LOG_INF
```

而不是：

```text
Zephyr shell backend + SHELL_CMD_REGISTER
```

这不是否定 Zephyr shell，而是当前项目更需要一个贴合自己调试方式的轻量控制面。

---

## 为什么 shell、buzzer、flash 有宏入口

当前 `cmd/` 的宏可以分成两类。

### 注册宏

`shell` 使用的是注册宏：

```cpp
REGISTER_SHELL_VAR("name", variable);
```

它的作用是把调试变量在编译期放入 `.shell_var` 段：

```text
业务文件
    -> REGISTER_SHELL_VAR()
        -> 生成 debug::Entry
            -> section(".shell_var")
                -> __shell_var_start / __shell_var_end
                    -> DbgConsole 遍历
```

注册宏解决的是“如何让调试变量分散声明、集中发现”。

如果不用注册宏，就需要一个中心表：

```cpp
DebugVar vars[] = {
    {"gain", &gain},
    {"offset", &offset},
    ...
};
```

这会带来两个问题：

- 新增变量要修改中心文件；
- 删除变量时容易忘记同步注册表。

注册宏把注册点放回变量所在文件，符合当前框架的“本地声明、本地拥有、链接期收集”思路。

### 执行宏

`buzzer` 和 `flash` 使用的是执行宏：

```cpp
EXEC_BUZZER_SHORT();
EXEC_BUZZER_ERR(nullptr);

EXEC_FLASH_READ(addr, data, len);
EXEC_FLASH_WRITE(addr, data, len);
```

执行宏解决的是“调用方能否无条件调用一个可选能力”。

例如 `Init_entry.cpp` 希望启动阶段结束时给一个声音提示：

```cpp
EXEC_BUZZER_SHORT();
```

如果没有执行宏，调用方就必须写：

```cpp
#ifdef CONFIG_CMD_BUZZER
buzzer::Instance().Short();
#endif
```

这个判断会散落到所有调用点。

执行宏把条件编译收回到 `cmd` 自己的头文件里：

```text
调用点保持简单
功能开关留在 cmd 内部
编译结果由 Kconfig 决定
```

### 宏不是为了偷懒，而是为了隔离条件编译

在嵌入式项目里，`#ifdef` 最容易扩散。

如果每个调用点都直接判断 `CONFIG_CMD_BUZZER`、`CONFIG_CMD_W25Q128`，后续会变成：

```text
业务代码知道太多配置名
配置变化导致大量文件修改
功能关闭时到处需要补 fallback
```

执行宏的价值就是让调用方只看到稳定动作：

```text
响一声
报警
读 flash
写 flash
注册变量
```

至于这个动作在当前固件里是真执行、空执行，还是返回失败，由 `cmd` 自己决定。

这是一种很实用的低耦合写法：

```text
业务依赖语义
cmd 依赖配置
```

而不是：

```text
业务到处依赖 CONFIG_XXX
```

---

## 为什么允许这些头文件常包含

当前代码里已经出现了这种模式。

`Init_entry.cpp` 无条件包含：

```cpp
#include "buzzer.hpp"
```

IMU 公共设备层无条件包含：

```cpp
#include "partition.hpp"
#include "w25q128.hpp"
```

这不是随意扩大 include，而是有意识地把 `cmd` 的公开入口做成“稳定头文件”。

### 头文件常包含的目的

头文件常包含的目的，是让上层不用关心功能是否启用。

调用方只写：

```cpp
EXEC_FLASH_READ(flash::kPartCalib.offset, &calib, sizeof(ImuOffsetData));
```

而不是写：

```cpp
#ifdef CONFIG_CMD_FLASH
EXEC_FLASH_READ(...);
#else
...
#endif
```

这样做可以减少三类耦合。

第一，减少调用方对 Kconfig 名称的耦合。

调用方不需要知道底层开关叫 `CMD_W25Q128`、`CMD_FLASH` 还是以后改成别的名字。

第二，减少调用方对实现类的耦合。

调用方不需要知道当前 Flash 是 `W25Q128`，以后如果换成别的 SPI NOR，宏入口可以保持不变。

第三，减少调用方对编译裁剪的耦合。

功能关闭时，调用方仍然能编译，只是宏展开为降级行为。

### 头文件常包含的前提

允许常包含不代表头文件可以随便写。

它必须满足几个规则。

#### 1. 关闭功能时也必须能展开

例如 `w25q128.hpp` 关闭时提供：

```cpp
#define EXEC_FLASH_READ(addr, data, len)  ((void)(addr), (void)(data), (void)(len), false)
#define EXEC_FLASH_WRITE(addr, data, len) ((void)(addr), (void)(data), (void)(len), false)
```

所以 IMU 可以始终调用 `EXEC_FLASH_READ()`。

Flash 关闭时，IMU 不会因为找不到 `w25q128::Instance()` 而链接失败，而是得到一个明确的 `false`。

#### 2. 关闭功能时不能暴露未定义类型

头文件里如果在 `#ifdef CONFIG_XXX` 内定义类，那么关闭分支里不能要求调用方继续使用这个类。

正确用法是让调用方只用宏：

```cpp
EXEC_FLASH_READ(...)
EXEC_BUZZER_SHORT()
REGISTER_SHELL_VAR(...)
```

调用方不应该在功能关闭时还直接写：

```cpp
w25q128::Instance().Read(...)
buzzer::Instance().Short()
debug::DbgConsole console
```

因为这些类型只在功能开启时存在。

#### 3. 公共数据常量应独立于具体驱动

`partition.hpp` 没有放进 `CONFIG_CMD_W25Q128` 里，这一点是合理的。

分区布局表达的是存储地址约定，不等于具体 Flash 驱动一定启用。

IMU 可以知道：

```cpp
flash::kPartCalib.offset
```

但不需要知道当前底层读写是 W25Q128、片内 Flash，还是以后换成别的存储后端。

这说明 `partition.hpp` 更像“存储命名空间和地址契约”，而 `w25q128.hpp/.cpp` 才是具体驱动。

#### 4. 源文件仍然必须由 Kconfig 裁剪

头文件常包含，不等于实现常编译。

当前 `cmd/CMakeLists.txt` 使用：

```cmake
if(CONFIG_CMD_BUZZER)
    target_sources(app PRIVATE buzzer/buzzer.cpp)
endif()

if(CONFIG_CMD_W25Q128)
    target_sources(app PRIVATE flash/w25q128.cpp)
endif()
```

这意味着：

- 头文件提供稳定接口；
- `.cpp` 只在功能开启时进入编译；
- 关闭功能时不会产生无用对象、线程和驱动依赖。

这就是“接口可见，实现在场与否可裁剪”。

#### 5. include 目录也要服务常包含

如果某个头文件被设计成无条件包含，那么它所在目录也应该在 CMake 中保持可见。

当前 `cmd/CMakeLists.txt` 对 `buzzer/` 和 `flash/` 已经是这种思路：

```cmake
target_include_directories(app PRIVATE buzzer)
target_include_directories(app PRIVATE flash)
```

`shell.hpp` 的头文件本身也有关闭分支：

```cpp
#define REGISTER_SHELL_VAR(name_, var_)
```

因此它在接口形态上同样适合被常包含。当前 `shell/` include 目录仍放在 `CONFIG_CMD_SHELL_VAR` 条件内，如果后续希望任意业务文件都能无条件写 `#include "shell.hpp"`，可以把 `shell/` 的 include 目录也调整为常开放，而源文件继续由 `CONFIG_CMD_SHELL_VAR` 裁剪。

---

## Kconfig 与 CMake 的关系

`cmd/Kconfig` 负责功能选择。

```text
CMD_SHELL_VAR
    select COM_UART_DMA

CMD_BUZZER
    select DEV_PWM

CMD_W25Q128
    select COM_SPI

CMD_FLASH
    select CMD_W25Q128
```

这表示：

- shell 需要 UART DMA；
- buzzer 需要 PWM；
- W25Q128 需要 SPI；
- Flash 分区能力当前依赖 W25Q128 后端。

`project/thread/Kconfig` 再提供项目侧入口：

```text
USE_CMD_SHELL
    select CMD_SHELL_VAR

USE_CMD_BUZZER
    select CMD_BUZZER

USE_CMD_FLASH
    select CMD_FLASH
```

这层的意义是让项目配置不直接记住 cmd 内部细节。

项目侧可以说：

```text
我要 shell
我要 buzzer
我要 flash
```

而不是必须知道它们分别 select 哪些底层驱动。

CMake 负责把 Kconfig 的结果变成真实编译单元：

```cmake
if(CONFIG_CMD_SHELL_VAR)
    target_sources(app PRIVATE shell/shell.cpp)
endif()

if(CONFIG_CMD_BUZZER)
    target_sources(app PRIVATE buzzer/buzzer.cpp)
endif()

if(CONFIG_CMD_W25Q128)
    target_sources(app PRIVATE flash/w25q128.cpp)
endif()
```

所以完整链路是：

```text
项目配置
    -> select CMD_XXX
        -> select 底层依赖
            -> CMake 添加源文件
                -> REGISTER_INIT / 注册宏进入链接段
                    -> 启动器或运行时遍历执行
```

---

## linker/ 的意义

`cmd/linker/tflm_init.ld` 当前集中定义多个链接段：

| 段 | 用途 |
| --- | --- |
| `.user_init` | `REGISTER_INIT()` 初始化项 |
| `.can_rx1` | CAN1 接收分发表 |
| `.can_rx2` | CAN2 接收分发表 |
| `.can_rx3` | CAN3 接收分发表 |
| `.remote` | Remote 协议注册表 |
| `.imu` | IMU 数据源注册表 |
| `.shell_var` | shell 调试变量注册表 |

这些段共同服务一个设计：

```text
功能在本地声明
    -> 编译器把条目放入指定 section
        -> linker 保留 section 并导出 start/end
            -> 运行时按边界遍历
```

这和 `cmd/` 的宏接口天然配合。

例如 shell：

```text
REGISTER_SHELL_VAR
    -> .shell_var
        -> __shell_var_start / __shell_var_end
            -> DbgConsole::CmdList / CmdGet / CmdSet
```

buzzer 和 flash：

```text
buzzer.cpp / w25q128.cpp
    -> REGISTER_INIT
        -> .user_init
            -> System_Startup()
                -> RunStage(PreInit)
```

这让 `cmd` 组件不需要中心注册表，也不需要在 `main.c` 里手动追加初始化函数。

---

## shell/ 细节

### 公开入口

`shell.hpp` 提供：

- `debug::VarType`
- `debug::Entry`
- `debug::DbgConsole`
- `debug::TypeMap<T>`
- `REGISTER_SHELL_VAR(name, var)`

实际业务最常用的只有：

```cpp
REGISTER_SHELL_VAR("name", variable);
```

### 支持类型

当前支持：

```text
uint8_t
int8_t
uint16_t
int16_t
uint32_t
int32_t
uint64_t
int64_t
float
double
bool
```

类型由：

```cpp
TypeMap<decltype(var_)>::type
```

在编译期推导。

这比字符串类型表更稳，因为变量类型变了，注册项也会跟着变。

### 命令

| 命令 | 功能 |
| --- | --- |
| `l` | 列出所有变量 |
| `g <name>` | 读取变量 |
| `s <name> <value>` | 修改变量 |
| `?` / `h` | 帮助 |

### 数据安全边界

当前 shell 直接保存变量指针，读写时没有自动加锁。

因此注册变量时应遵守几个约束：

- 优先注册简单标量；
- 不要注册临时变量；
- 不要注册生命周期短于整个固件的局部对象；
- 不要直接注册复杂结构体、大数组或硬件句柄；
- 被高频控制环同时读写的变量，调用方应自己考虑原子性或临界区；
- shell 更适合作为调试参数入口，不应该承担安全关键控制。

这不是缺陷，而是这个工具的边界：它追求低开销、低侵入、快速调试。

---

## buzzer/ 细节

`buzzer/` 提供一个基于 PWM 的全局蜂鸣器对象。

### 公开入口

功能开启时：

```cpp
EXEC_BUZZER_SHORT()  -> buzzer::Instance().Short()
EXEC_BUZZER_LONG()   -> buzzer::Instance().Long()
EXEC_BUZZER_ERR(...) -> buzzer::Instance().Err(...)
```

功能关闭时：

```cpp
EXEC_BUZZER_SHORT()  -> 空
EXEC_BUZZER_LONG()   -> 空
EXEC_BUZZER_ERR(...) -> while(1) {}
```

这里有一个很重要的设计：`EXEC_BUZZER_ERR()` 在关闭蜂鸣器时不是空操作，而是保留停机语义。

因为它当前用于初始化失败路径：

```cpp
if (halt) {
    EXEC_BUZZER_ERR(nullptr);
    while (1) {}
}
```

如果蜂鸣器功能没开，系统仍然应该停在错误现场，而不是因为少了蜂鸣器就继续跑。

所以 buzzer 宏不是单纯提示音封装，它还承接了“错误停机入口”的语义。

### 初始化

`buzzer.cpp` 中注册：

```cpp
REGISTER_INIT(buzzer_init, PreInit, Mid, "buzzer");
```

只要 `CONFIG_CMD_BUZZER=y`，`buzzer.cpp` 进入编译，初始化项就进入 `.user_init`。

如果 `CONFIG_CMD_BUZZER=n`，`buzzer.cpp` 不编译，初始化项自然不存在。

调用点仍然可以包含 `buzzer.hpp`，因为宏会提供关闭分支。

### 使用边界

`Short()`、`Long()`、`Beep()` 当前是阻塞式接口，内部会 `k_msleep()`。

因此它适合：

- 启动阶段提示；
- 错误状态提示；
- 手动测试；
- 低频状态反馈。

不适合：

- 高频控制环；
- 中断上下文；
- 对时序敏感的实时任务。

---

## flash/ 细节

`flash/` 当前包含两层概念。

### 分区布局

`partition.hpp` 定义：

```cpp
flash::kPartCalib
flash::kPartLog
flash::kPartReserve
```

它是地址契约，不是具体芯片驱动。

例如 IMU 只需要知道校准数据存放在：

```cpp
flash::kPartCalib.offset
```

它不应该关心底层是 W25Q128，还是未来换成其他存储后端。

### W25Q128 后端

`w25q128.hpp/.cpp` 封装 SPI NOR Flash：

- JEDEC ID；
- Unique ID；
- Read / FastRead；
- PageWrite / Write；
- SectorErase；
- BlockErase；
- ChipErase；
- PowerDown / Wakeup；
- ResetDevice；
- Busy 等待。

初始化项：

```cpp
REGISTER_INIT(flash_init, PreInit, Mid, "w25q128");
```

Flash 当前是同步阻塞实现。

这意味着它适合：

- 启动时读取参数；
- 校准完成后保存参数；
- 离线测试；
- 日志或配置写入的低频路径。

它不适合直接放进高频控制环里反复擦写。

### 执行宏

功能开启时：

```cpp
EXEC_FLASH_READ(addr, data, len)
EXEC_FLASH_WRITE(addr, data, len)
EXEC_FLASH_SECTOR_ERASE(addr)
EXEC_FLASH_CHIP_ERASE()
EXEC_FLASH_POWER_DOWN()
EXEC_FLASH_WAKEUP()
```

功能关闭时统一返回 `false`。

这个选择让调用方可以自然写：

```cpp
if (EXEC_FLASH_READ(...)) {
    // 使用持久化数据
} else {
    // 使用默认值
}
```

所以 Flash 关闭不是编译错误，而是一种明确的运行能力缺失。

---

## build/ 细节

`build/` 是主机侧构建脚本，不进入固件。

当前仓库真正的板级装配规则仍以根 `CMakeLists.txt` 为准：

```text
project/boards/*/<BOARD_CFG>/<BOARD>.overlay
project/boards/*/<BOARD_CFG>/<BOARD>.conf
project/boards/*/<BOARD_CFG>/board.cmake
```

`build.bat` 和 `build.ps1` 只是 `west build` 的封装入口，方便在 Windows 下快速选择板级配置。

文档或脚本如果和根 CMake 的真实目录不一致，应以根 CMake 为准。

---

## 设计原则

### 1. 公开头文件稳定，具体实现可裁剪

调用方应依赖：

```text
REGISTER_SHELL_VAR
EXEC_BUZZER_XXX
EXEC_FLASH_XXX
flash::Partition
```

而不是依赖具体实现文件是否编译。

这样可以做到：

```text
功能打开
    -> 宏执行真实动作

功能关闭
    -> 宏提供空操作或 false

调用方
    -> 不需要散落 #ifdef
```

### 2. init 放在 .cpp，不放在调用方

`buzzer.cpp` 和 `w25q128.cpp` 都在自己的实现文件里写 `REGISTER_INIT()`。

这样新增一个 `cmd` 组件时，不需要改 `main.c`，也不需要改 `Init_entry.cpp`。

启用该组件时：

```text
Kconfig=y
    -> CMake 编译 .cpp
        -> REGISTER_INIT 进入 .user_init
            -> System_Startup 自动执行
```

关闭该组件时：

```text
Kconfig=n
    -> .cpp 不编译
        -> 初始化项不存在
            -> 调用宏走关闭分支
```

### 3. 宏只封装横切动作，不封装复杂业务

适合宏封装的能力：

- 启动提示；
- 错误报警；
- 参数读写；
- 调试变量注册；
- 可选调试输出；
- 低频辅助动作。

不适合宏封装的能力：

- 底盘控制策略；
- 云台状态机；
- IMU 姿态解算；
- Remote 解码逻辑；
- 电机闭环计算；
- topic 消息结构。

一句话：

```text
宏适合屏蔽“功能是否存在”
宏不适合隐藏“业务到底怎么运行”
```

### 4. cmd 可以被多个层调用，但不能反向污染多个层

`cmd` 的头文件可以被 `project/`、`modules/` 调用。

但 `cmd` 自己不应该认识：

- 某个底盘线程；
- 某个云台线程；
- 某个遥控器协议；
- 某台机器人专属业务。

这能保证 `cmd` 是平台工具，而不是另一个业务中心。

---

## 新增 cmd 组件的建议流程

如果后续新增一个 `cmd/foo`，建议遵守下面的形态。

### 1. 头文件提供稳定宏入口

```cpp
#pragma once

#ifdef CONFIG_CMD_FOO

namespace foo {
class Foo { ... };
Foo& Instance();
}

#define EXEC_FOO_DO(...) foo::Instance().Do(__VA_ARGS__)

#else

#define EXEC_FOO_DO(...) false

#endif
```

要求：

- 开启时能调用真实实现；
- 关闭时仍然能编译；
- 关闭语义要明确，是空操作、返回 false，还是停机；
- 调用方不需要写 `#ifdef CONFIG_CMD_FOO`。

### 2. 源文件自己注册初始化项

```cpp
#include "foo.hpp"
#include "Init_entry.hpp"

static bool foo_init()
{
    return foo::Instance().Init();
}

REGISTER_INIT(foo_init, PreInit, Mid, "foo");
```

要求：

- 初始化注册写在 `foo.cpp`；
- 不修改 `main.c`；
- 不修改 `Init_entry.cpp`；
- 是否存在由 CMake 和 Kconfig 决定。

### 3. Kconfig 声明功能和依赖

```kconfig
config CMD_FOO
    bool "Enable foo command helper"
    default n
    select DEV_XXX
```

要求：

- `CMD_FOO` 表达 cmd 内部功能；
- 项目层可以再提供 `USE_CMD_FOO`；
- 底层依赖用 `select` 明确拉起。

### 4. CMake 只裁剪源文件，不要求调用方改代码

```cmake
target_include_directories(app PRIVATE foo)

if(CONFIG_CMD_FOO)
    target_sources(app PRIVATE foo/foo.cpp)
endif()
```

要求：

- 如果头文件设计为常包含，include 目录应保持可见；
- `.cpp` 源文件由 `CONFIG_CMD_FOO` 控制；
- 关闭后不产生对象代码和初始化项。

---

## 最终判断

当前 `cmd/` 的定位可以概括为：

```text
cmd 是框架里的可选执行工具层。
它允许上层长期包含稳定入口，
同时让具体实现通过 Kconfig/CMake 裁剪。
```

shell 独立维护线程，是因为它是调试控制面，不是项目业务线程。

buzzer 和 flash 使用执行宏，是为了让初始化、IMU 校准、错误处理等调用点不被 `#ifdef` 污染。

这些头文件允许常包含，是因为关闭功能时它们仍然提供明确的降级语义。

这套设计真正服务的是同一个目标：

```text
新增辅助能力时，不扩大中心文件；
关闭辅助能力时，不破坏调用方；
替换底层实现时，不牵动上层业务。
```

这就是 `cmd/` 在当前 tflm 架构中的价值。
