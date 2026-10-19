# shell 调试台模块（log / var / shell 底座）

> 一套自研的 DMA 日志系统（DUST_LOG）与自维护 UART shell 融合的统一调试台。
> 模块分工：**log**（日志打印 + 通道策略 + 帧队列仲裁 + 参数快照异步格式化 + 发送泵）、**var**（调试变量注册与读写）、**shell**（UART 线程底座 + 命令表分派 + 日志发送驱动）、**thread**（线程运行统计与暂停/恢复）、**boot**（启动期耗时查询）。
> 命令族通过 **链接段注册**（`REGISTER_SHELL_CMD` → `.shell_cmd`），新增命令族不需要修改 `shell.cpp`。
> 替代 Zephyr log，根治 uart3 回调槽冲突卡死。

---

## 目录

1. [模块总览](#1-模块总览)
2. [快速上手](#2-快速上手)
3. [log 模块详解](#3-log-模块详解)
4. [var 模块详解](#4-var-模块详解)
5. [shell 底座详解](#5-shell-底座详解)
6. [命令族注册与分派](#6-命令族注册与分派)
7. [thread 命令族](#7-thread-命令族)
8. [boot 命令族](#8-boot-命令族)
9. [命令参考](#9-命令参考)
10. [容量模型与实测数据](#10-容量模型与实测数据)
11. [FAQ](#11-faq)

---

## 1. 模块总览

```text
shell/
├── shell.hpp / shell.cpp       底座：UART 线程 + 命令表分派 + 日志发送驱动
├── shell_cmd.hpp               命令族注册宏 + 段边界访问
├── token.hpp                   行切分工具（NextToken / SkipSpace）
├── boot/                       boot 命令（启动期耗时查询）
├── log/                        日志子系统（门面 + 策略 + 队列 + 发送泵）
├── var/                        调试变量（链接段收集 + var 命令）
└── thread/                     thread 命令（线程运行统计与暂停/恢复）
```

| 模块 | 文件 | 职责 | Kconfig 符号 |
|------|------|------|-------------|
| log | [log/](log/) 见下方分层表 | 日志门面 + 通道策略 + DBG 源注册 + 记录/格式化 + 队列/仲裁 + 发送泵 | `DUST_CMD_SHELL_LOG` |
| var | [var/var.hpp](var/var.hpp) / [var/var.cpp](var/var.cpp) | 调试变量注册（`.shell_var` 链接段收集）+ var list/get/set | `DUST_CMD_SHELL_VAR` |
| thread | [thread/thread_cmd.hpp](thread/thread_cmd.hpp) / [thread/thread_cmd.cpp](thread/thread_cmd.cpp) | `thread list/show/pause/resume/reset`，遍历 `thread_registry` 输出运行统计与控制线程 | `DUST_CMD_SHELL` |
| boot | [boot/boot_cmd.hpp](boot/boot_cmd.hpp) / [boot/boot_cmd.cpp](boot/boot_cmd.cpp) | `boot timing`，遍历 `.boot_timing` 段输出启动期各项耗时 | `DUST_CMD_SHELL` |
| shell | [shell.hpp](shell.hpp) / [shell.cpp](shell.cpp) | UART 线程底座（接收循环 + 命令表分派 + 日志发送驱动）+ 初始化接入 | `DUST_CMD_SHELL` |
| 命令表 | [shell_cmd.hpp](shell_cmd.hpp) | `REGISTER_SHELL_CMD` 宏 + `.shell_cmd` 段遍历 | `DUST_CMD_SHELL` |
| 行切分 | [token.hpp](token.hpp) | `NextToken` / `SkipSpace`，各命令族共用 | `DUST_CMD_SHELL` |

**log 分层（全部头文件内联实现，只有一个 .cpp）**：

| 文件 | 层 | 内容 |
|------|----|------|
| [log/log.hpp](log/log.hpp) | 门面 | `Log` 类 + `DUST_LOG_*` 宏族 |
| [log/log_policy.hpp](log/log_policy.hpp) | 策略 | `LogColor` / `TxPriority` / `LogChannel` + `LogChannelPolicy` / `TxPriorityPolicy` 表 |
| [log/log_debug.hpp](log/log_debug.hpp) | DBG 源注册 | `LogEntry` + `DebugSourceRegistry`（名字/选中/选择代数/输出模式） |
| [log/log_record.hpp](log/log_record.hpp) | 记录与格式化 | `LogRecord` + `BuildRecordFromVaList` / `FormatRecordToText` / `WrapOutputFrame` |
| [log/log_queue.hpp](log/log_queue.hpp) | 队列与仲裁 | `TxFrame` + `LogRecordQueue` / `TxFrameQueue` |
| [log/log_transport.hpp](log/log_transport.hpp) | 发送泵 | `LogTransport`（通道绑定 / DMA 空闲续发 / 发送完成回调） |
| [log/log.cpp](log/log.cpp) | 门面实现 | `Log` 各入口实现 + `log` 命令解析 |

**依赖链**：

```
USE_CMD_SHELL → select DUST_CMD_SHELL_LOG ─┐
             → select DUST_CMD_SHELL_VAR ─┴→ DUST_CMD_SHELL → DUST_COM_UART_DMA
```

- 业务层 `USE_CMD_SHELL` 是总开关：select `DUST_CMD_SHELL_LOG` + `DUST_CMD_SHELL_VAR`，二者再各自 select `DUST_CMD_SHELL`
- `DUST_CMD_SHELL` 是公共底座（线程 + 分发），`DUST_CMD_SHELL_VAR` / `DUST_CMD_SHELL_LOG` 各自 select 它
- `log` 依赖 `shell`：`Log::Init()` / `Log::BindOutputStream()` 在 shell 的 `dbg_init` 里执行——没有 shell 就没人初始化 Log（通道为 `nullptr` 时发送静默丢弃）；`log` 命令也走 shell 的 `ProcessLine` 分发

**命令分派（链接段注册）**：命令族不写在 `shell.cpp` 里，而是由各模块用
`REGISTER_SHELL_CMD(name, help, handler, ident)` 注册到 `.shell_cmd` 段。
`ProcessLine` 取第一个 token 后遍历该段匹配，命中即把剩余参数串交给 handler。
`h` 的输出同样来自遍历，新增命令族不需要改 `shell.cpp`，帮助自动出现。

**关键设计（发送路径唯一）**：log 与 shell 共用同一 `Stream` 的 DMA 发送通道——shell `thread_init` 注册 `tx_cb = Log::HandleTxDone`，log 发送帧经由同一个 `Stream`。不碰 Zephyr `uart_callback_set` 槽位，天然无冲突。

**通道抽象（Stream）**：shell 与 log 都持 `Stream*` 而非具体 `UartDma*`。`UartDma`、未来的 `Usb`、`RS485` 都是 `Stream` 子类——调试通道可整体替换，类型不动。

---

## 2. 快速上手

### 打日志

```cpp
#include "log.hpp"

DUST_LOG_INF("vx=%.2f", vx);              // 黑色 [inf]，一次性
DUST_LOG_ERR("can tx fail %d", ret);      // 亮红 [err]，一次性
DUST_LOG_OK("power in budget");           // 亮绿 [ok]，一次性
DUST_LOG_WRN("imu drift");                // 亮橙 [wrn]，一次性
DUST_LOG_DBG("test_vx", "vx=%.2f", vx);   // 薄荷绿，带名字——默认静默，log on test_vx 后流式打印
```

### 注册调试变量

```cpp
#include "var.hpp"

static float g_vx = 0.0f;
REGISTER_SHELL_VAR("vx", g_vx);           // 文件级变量

// 类成员也可以：
class Chassis { public: float vx_ = 0.0f; };
static Chassis g_chassis {};
REGISTER_SHELL_VAR("chassis_vx", g_chassis.vx_);   // 取成员地址 + TypeMap 推导
```

### 命令

```
h                       帮助（遍历 .shell_cmd 段，每族一行）
var list                列出所有调试变量
var get <name>          查看变量
var set <name> <val>    修改变量
log list                列出所有 DBG 日志条目（含选中状态与输出模式）
log on <name>           选中一条日志流式打印（同一时间只打一条）
log off                 停止流式打印
log mode <name> on|off  切换输出模式（on=VOFA+纯文本，off=上色，默认上色）
thread list             所有线程的运行统计
thread show <name>      单线程详细统计（含耗时与栈剩余）
thread pause <name>     暂停线程（保留现场）
thread resume <name>    继续执行
thread reset <name>     清零计数器
boot timing             启动期各项耗时
```

### 注册命令族

```cpp
#include "shell_cmd.hpp"

static void MyCmd(uint8_t* args)
{
    // args 是「命令名之后」的原始参数串
}

REGISTER_SHELL_CMD("mycmd", "mycmd 说明", MyCmd, mycmd);
```

---

## 3. log 模块详解

### 3.1 枚举与常量（[log_policy.hpp](log/log_policy.hpp)）

```cpp
// 颜色：TrueColor（24 位 RGB），输出 \x1b[38;2;R;G;Bm
enum class LogColor : uint32_t
{
    Black  = 0x000000,  // 黑（INF）
    Red    = 0xF50002,  // 亮红（ERR）
    Green  = 0x00F700,  // 亮绿（OK）
    Orange = 0xF6A753,  // 亮橙（WRN）
    Mint   = 0x7BF7CD,  // 薄荷绿（DBG）
};

// 发送优先级：数值越小越靠前发
enum class TxPriority : uint8_t
{
    Event = 0,   // INF/ERR/OK/WRN：最高，插队头，永不挤
    Cmd   = 1,   // 命令响应（var/log 输出）：中，插事件后、DBG 前
    Dbg   = 2,   // DBG 流式：最低，排队尾，先被挤
};

// 日志语义通道（对外入口按通道取策略）
enum class LogChannel : uint8_t { Info, Error, Ok, Warning, Debug, Command };
```

**通道策略表**（`kLogChannelPolicies[]` / `GetLogChannelPolicy(channel)`）：

```cpp
struct LogChannelPolicy
{
    LogChannel  channel;          // 通道
    TxPriority  priority;         // 展开后的发送优先级
    LogColor    color;            // ANSI 颜色
    const char* prefix;           // 行首前缀
    bool        ansi_color;       // 通道默认是否上 ANSI 颜色（DBG 实际形态由条目标志覆盖）
    bool        selectable;       // 是否可被 log on/off 选择
    bool        stale_on_switch;  // 切换选择时是否作废在途记录
    bool        evictable;        // 池满时是否可被挤出
};

inline constexpr LogChannelPolicy kLogChannelPolicies[] = {
    { LogChannel::Info,    TxPriority::Event, LogColor::Black,  "[inf] ", true,  false, false, false },
    { LogChannel::Error,   TxPriority::Event, LogColor::Red,    "[err] ", true,  false, false, false },
    { LogChannel::Ok,      TxPriority::Event, LogColor::Green,  "[ok] ",  true,  false, false, false },
    { LogChannel::Warning, TxPriority::Event, LogColor::Orange, "[wrn] ", true,  false, false, false },
    { LogChannel::Debug,   TxPriority::Dbg,   LogColor::Mint,   "",       true,  true,  true,  true  },
    { LogChannel::Command, TxPriority::Cmd,   LogColor::Black,  "",       false, false, false, false },
};
```

**发送优先级策略表**（`kTxPriorityPolicies[]` / `GetTxPriorityPolicy(prio)`）—— 池满时挤谁不看枚举数值，看这张表：

```cpp
struct TxPriorityPolicy
{
    TxPriority priority;         // 优先级档位
    bool       may_be_evicted;   // 池满时是否可被挤出
    bool       may_evict_lower;  // 池满时是否可挤出更低档
    bool       preserve_fifo;    // 同档是否保持 FIFO
};

inline constexpr TxPriorityPolicy kTxPriorityPolicies[] = {
    { TxPriority::Event, false, true, true },   // 事件永不挤
    { TxPriority::Cmd,   true,  true, true },
    { TxPriority::Dbg,   true,  true, true },
};
```

**常量总表**：

| 常量 | 值 | 定义处 | 含义 |
|------|-----|--------|------|
| `kLogBufSize` | 256 | [log_record.hpp](log/log_record.hpp) | 格式化缓冲：可变参数展开目标（超 255 截断） |
| `kColorOutExtra` | 16 | [log_record.hpp](log/log_record.hpp) | 颜色输出缓冲额外预留（ANSI 转义 + `\r\n`） |
| `kLineOutExtra` | 4 | [log_record.hpp](log/log_record.hpp) | 行输出缓冲额外预留（`\r\n`） |
| `kMaxLogArgs` | 4 | [log_record.hpp](log/log_record.hpp) | 参数快照槽上限（`%f` 占 2 槽） |
| `kMaxLogRecords` | 8 | [log_queue.hpp](log/log_queue.hpp) | LogRecord 原始请求队列深度 |
| `kTxFrameSize` | 128 | [log_queue.hpp](log/log_queue.hpp) | 帧数据区大小（含尾部 `\0` 保险） |
| `kTxMaxLen` | 127 | [log_queue.hpp](log/log_queue.hpp) | 发送最长字节长度，超长截断 |
| `kTxPoolCount` | 8 | [log_queue.hpp](log/log_queue.hpp) | 帧池帧数（8 × 128B = 1KB） |
| `kNullIndex` | 255 | [log_queue.hpp](log/log_queue.hpp) | 索引链表空值（255 = 无下一项） |
| `kMaxLogEntries` | 64 | [log_debug.hpp](log/log_debug.hpp) | DBG 条目数上限 |

**每帧字节开销**（TrueColor 上色路径）：

```
[inf]前缀(5) + 颜色 \x1b[38;2;R;G;Bm(最长18) + 复位 \x1b[0m(4) + \r\n(2) = 最长 29B
帧上限 127B → 内容建议 ≤98B（超出截尾）
```

### 3.2 数据结构

```cpp
struct TxFrame        // 发送帧（三档共用，已格式化）——log_queue.hpp
{
    char       data[kTxFrameSize];   // 格式化后内容（含 ANSI 颜色），超 kTxMaxLen 截断
    uint16_t   len;                  // 实际有效长度（≤127）
    TxPriority prio;                 // 优先级档位
    uint16_t   epoch;                // DBG 选择代数（出队比对，不一致丢弃）
    uint8_t    next;                 // 帧索引链表（255=nullptr）
};

struct LogEntry       // DBG 日志条目（只服务 DBG）——log_debug.hpp
{
    const char* name;                // DBG 名字（log on 用，运行时 FindOrCreate 创建）
    bool        vofa;                // 输出模式：false=上色（默认）/ true=VOFA+ 纯文本
};

struct LogRecord      // 日志原始请求（未格式化，异步段生产队列条目）——log_record.hpp
{
    const char* fmt;                 // 格式串（静态字面量，禁止临时栈串）
    LogChannel  channel;             // 日志通道（通道策略表索引）
    LogColor    color;               // 颜色（由通道策略填入）
    TxPriority  prio;                // 展开后的发送优先级
    uint16_t    epoch;               // DBG 选择代数（出队比对，不一致丢弃）
    bool        ansi;                // 是否上 ANSI 颜色（DBG 按条目模式，其余取通道策略）
    uint32_t    args[kMaxLogArgs];   // 参数快照（%f 占 2 槽，%s/%p 指针 1 槽，整型 1 槽）
    uint8_t     nargs;               // 参数槽数（0~kMaxLogArgs）
    uint8_t     next;                // 记录链表（数组索引，255=nullptr）
};
```

`DebugSourceRegistry`（[log_debug.hpp](log/log_debug.hpp)）只管**名字与状态**，不感知队列/优先级/格式化：

```cpp
class DebugSourceRegistry
{
public:
    void Reset();                              // 清选中/计数/选择代数
    LogEntry* FindOrCreate(const char* name);  // 按名字查，首见创建（nullptr=池满）
    bool Select(const char* name);             // 选中已存在条目（++epoch_）
    bool SetVofa(const char* name, bool vofa); // 设置输出模式（不存在返回 false）
    void Deselect() { active_ = nullptr; ++epoch_; }
    const LogEntry* Active() const;            // 当前选中条目
    uint16_t ActiveEpoch() const;              // 当前选择代数
    LogEntry* First(); LogEntry* Next(const LogEntry* e);   // log list 遍历
private:
    LogEntry  entries_[kMaxLogEntries] {};
    LogEntry* active_ = nullptr;
    uint8_t   count_  = 0;
    uint16_t  epoch_  = 0;                     // 选择代数（Select/Deselect 递增）
};
```

### 3.3 链表机制：三类链，零动态分配

整个日志系统的队列全部是**静态数组 + 数组索引链表**（`uint8_t next` 存数组下标，`255`=空），运行期不 new/malloc。

**① `TxFrameQueue`（[log_queue.hpp](log/log_queue.hpp)）**：内部维护 `frames/free_head/head/tail`，负责发送帧池、按档插队、池满挤帧、按 epoch 跳过作废 DBG 帧。

**② `LogRecordQueue`（[log_queue.hpp](log/log_queue.hpp)）**：内部维护 `records/free_head/head/tail`，负责异步段未格式化请求的 FIFO、池满丢弃、按 epoch 跳过作废 DBG 记录，以及格式化完成后的回收。

帧在 `TxFrameQueue` 内流转（分配→入队→发送→回收），`LogRecord` 在 `LogRecordQueue` 内流转（快照入队→shell 出队→格式化入帧池→回空闲）。静态池 + 索引链表 = 时间确定、无碎片；封装后 `Log` 只表达业务动作。

### 3.4 打印入口（分时段）

| 宏 | 门面函数 | 通道 | 行为 |
|----|---------|------|------|
| `DUST_LOG_INF` | `Log::PrintInfo` | Info | `PrintColoredLog(Info, ...)` |
| `DUST_LOG_ERR` | `Log::PrintError` | Error | 同上，色不同 |
| `DUST_LOG_OK` | `Log::PrintOk` | Ok | 同上 |
| `DUST_LOG_WRN` | `Log::PrintWarning` | Warning | 同上 |
| `DUST_LOG_DBG(name, ...)` | `Log::PrintSelectedDebug(e, ...)` | Debug | `e != dbg_.Active()` 静默；否则按**条目标志**决定形态 |
| （内部） | `Log::SendCommandLine` | Command | 命令响应直发（带 `\r\n`） |

`PrintColoredLog` 内部按 **`shell_own_` 分时段**（详见 §3.5）：

```cpp
void Log::PrintColoredLog(LogChannel channel, const char* fmt, va_list ap)
{
    const LogChannelPolicy& policy = GetLogChannelPolicy(channel);

    if (!shell_own_)                          // 同步段（boot 早期）：调用点直发，现状不变
    {
        char buf[kLogBufSize];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        char out[kLogBufSize + kColorOutExtra];
        int n = WrapOutputFrame(policy.prefix, policy.ansi_color, policy.color, buf, out, sizeof(out));
        if (n > 0) QueueFrameAndSendNow(out, n, policy.priority, 0);
        return;
    }

    // 异步段（shell 接管后）：参数快照入队，调用点不格式化
    uint32_t args[kMaxLogArgs];
    uint8_t  nargs = 0;
    BuildRecordFromVaList(fmt, ap, args, &nargs);
    QueueLogRecord(fmt, channel, 0, policy.ansi_color, args, nargs);
    unsigned key = irq_lock();
    if (!pump_.Sending() && pump_.Channel() != nullptr) k_sem_give(&pump_.Channel()->sem_);
    irq_unlock(key);
}
```

### 3.5 DBG 输出模式标志（上色 / VOFA+）

DBG 流的输出形态**按名字**独立配置，默认上色。

| 标志 `LogEntry::vofa` | 形态 | 输出示例 |
|----------------------|------|---------|
| `false`（默认） | **上色**（ANSI TrueColor，薄荷绿） | `\x1b[38;2;123;247;205m1.23,4.56\x1b[0m\r\n` |
| `true` | **VOFA+ 纯文本**（FireWater 可解析） | `1.23,4.56\r\n` |

链路：

```
DUST_LOG_DBG("name", ...)
  → LogEntry::vofa（按名字，log mode <name> on|off 切换）
  → 入队时快照为 LogRecord::ansi = !vofa
  → shell 线程 FormatOnePendingRecord → WrapOutputFrame(policy.prefix, r->ansi, r->color, ...)
```

- **快照而非引用**：模式在入队时定死进 `LogRecord::ansi`，出队时不再查条目。
- **切换即时生效**：切换后新入队的记录立即用新模式（DBG 每周期都在入队，感受等同即时）。
- **不受 `log on/off` 影响**：模式与选中是两件事，`log off` 只停打印、不改模式。
- **为什么默认上色**：DBG 与 INF/ERR/OK/WRN 一样是给人看的调试输出，默认在终端里应当可读；只有明确要喂 VOFA+ 绘图时才 `log mode <name> on`。
- **VOFA+ 注意**：ANSI 转义串 `\x1b[38;2;R;G;Bm` 里的数字会被 VOFA+ 误解析成通道数据，所以 vofa 模式必须关闭上色。

### 3.6 分时段同步异步发送（核心机制）

**背景**：shell 线程在 `PreThread` 阶段才启动（[shell.cpp](shell.cpp) `REGISTER_THREAD(thread_start, PreThread, HaltOnFail, ...)`）。boot 早期 shell 线程还没跑，若调用点只入队会无人消费（帧积压满后吞日志）。所以用 `shell_own_` 标志划分两个时段：

| 时段 | `shell_own_` | 调用点行为 | 实时要求 |
|------|-------------|-----------|---------|
| **同步段**（boot 早期） | `false` | `vsnprintf` + 入帧池 + **调用点直发** | 无（启动阶段） |
| **异步段**（shell 接管后） | `true` | **参数快照**入 LogRecord 队列 + give `sem_`，不格式化 | 有（ISR/实时任务可能在此调用） |

- `shell_own_` 在 `Shell::Task()` 前由 `thread_start` 调 `Log::MarkShellThreadReady()` 置 `true`（[shell.cpp](shell.cpp)），只执行一次。
- **同步段日志即时出、不积压**（调用点直接 DMA 提交），避免 boot 日志吞帧。
- **异步段 ISR 只做快照+入队+give**（~µs 级），格式化与 DMA 全在 shell 线程。

### 3.7 发送仲裁（核心）

#### TxFrameQueue：帧池 + 三档优先级队列

真正的帧池分配、池满挤出、优先级插队都在 `TxFrameQueue` 内部完成（[log_queue.hpp](log/log_queue.hpp)）：

```cpp
bool PushFrameLocked(const char* data, int len, TxPriority prio, uint16_t epoch, bool allow_evict);
```

步骤：截断 → 从 `free_head` 取帧 → 池满处理（ISR 直接丢 / 线程可挤最低优先级）→ 拷内容 → `InsertFrameByPriorityLocked()` 按档插队。

规则：
- **Event(0) 插队头**——最高，永远最先发。
- **Cmd(1) 插事件后、DBG 前**。
- **Dbg(2) 排队尾**——最低。
- **同档先来后到**：`while (*pp != kNullIndex && frames[*pp].prio <= f->prio)` 跳过同档，插到第一个更低优先级帧之前。

#### EvictLowestPriorityFrameLocked：池满挤帧

遍历队列，取 **`GetTxPriorityPolicy(prio).may_be_evicted == true`** 中 prio 值最大（最低优先）的帧，从链表摘除返回。

- **事件永不挤**（策略表 `Event.may_be_evicted = false`）。
- 优先挤 DBG，其次 Cmd（两者 `may_be_evicted = true`）。被挤掉的帧直接复用为新帧。
- 新优先级只需改**策略表**，不用改这个函数。

#### LogTransport：发送泵（[log_transport.hpp](log/log_transport.hpp)）

```cpp
class LogTransport
{
public:
    void Reset();                          // 清「发送中」标志（初始化）
    void Bind(Stream* s);                  // 绑定发送通道
    bool Sending() const;                  // 当前是否有帧在 DMA 中
    Stream* Channel() const;               // 发送通道（唤醒 shell 泵用）

    // DMA 空闲则从队列取一帧提交发送，帧内容移交后立即归还帧池（调用者持 irq_lock）
    bool PumpTxQueue(TxFrameQueue& queue, uint16_t active_epoch);

    // TX_DONE 回调（UartDma tx_cb）：清标志 + 唤醒 shell 续发
    void HandleTxDone();
};
```

**关键点：帧即取即还**——当前 Stream 子类（如 `UartDma::Send`）内部把帧内容拷进自己的发送缓冲，DMA 搬运的是 Stream 子类缓冲而非 `TxFrame::data`。Send 提交成功即帧使命结束，立即回空闲池，**帧池稳态不被 DMA 占住**。

**发送驱动在 shell 线程**：异步段调用点只入队 + give；`HandleTxDone` ISR 只清标志 + give；shell `Task()` 唤醒后 `SendNextFrameIfIdle()` 提交下一帧。ISR 里不续发链。

#### 出队作废：source / epoch

选择切换（`log on` / `log off`）不再遍历队列打作废标记，改为**出队比对选择代数**：

```cpp
LogRecord* PopNextRecordLocked(uint16_t active_epoch);   // 记录：Dbg 档且 epoch 不符 → 回收跳过
TxFrame*   PopNextFrameLocked(uint16_t active_epoch);    // 帧：同上
```

- `DebugSourceRegistry` 每次 `Select`/`Deselect` 都 `++epoch_`。
- DBG 记录/帧在创建时携带当时的 epoch；出队时与 `dbg_.ActiveEpoch()` 不一致就丢弃。
- 好处：DBG 控制层**不需要知道队列内部结构**，切换零遍历。代价是最多残留「已在 DMA 中的一帧」无法取消（可接受）。

### 3.8 参数快照与格式化（[log_record.hpp](log/log_record.hpp)）

**目标**：异步段调用点（可能 ISR）连 vsnprintf 都不做，只把 `fmt + 参数值` 快照入队，格式化在 shell 线程。

**为什么快照而非存 va_list**：`va_list` 是栈指针，调用点函数一返回栈即回收，跨线程延迟使用是野指针。快照存的是**值拷贝**，不依赖调用点栈存活。

三个函数分工明确（快照 / 展开 / 包装），改动一处必看另两处：

#### BuildRecordFromVaList：参数值快照（调用点，va_end 前）

```cpp
inline void BuildRecordFromVaList(const char* fmt, va_list ap, uint32_t* args, uint8_t* nargs);
```

- **类型严格匹配 va_arg**，防失步；超 `kMaxLogArgs` 停止写槽但**继续消费参数**。
- **`%f` 占 2 槽**：double 64 位拆成低/高 32 位存两槽，展开时拼回。
- 支持转换符：`%d %i %u %x %X %c %f %F %s %p`。`%%` 无参数。禁用 `%lld/%e/%g/%n`。

#### FormatRecordToText：纯文本展开（shell 线程）

```cpp
inline size_t FormatRecordToText(const LogRecord& r, char* text, size_t text_size);
```

- 扫 fmt：普通字符直接拷；`%` 段收集到 `seg`，按转换符从 args 取槽 snprintf 展开。
- 用 picolibc snprintf 逐段展开（`%f` 精度由 libc 保证），不依赖 libc va_list。
- **不含前缀/颜色/行尾**，只管正文。
- **`%s` 约束**：快照存指针，延迟格式化要求指针仍有效——**只许字符串字面量**（静态存储期）。临时栈 `char buf[]` 传入会读野指针（最高风险，文档约束 + review 把关）。

#### WrapOutputFrame：行包装

```cpp
inline int WrapOutputFrame(const char* prefix, bool ansi_color, LogColor color,
                           const char* text, char* out, size_t out_size);
```

`ansi_color=false` → `prefix + text + \r\n`；`true` → `\x1b[38;2;R;G;Bm` + `prefix + text` + `\x1b[0m\r\n`。

#### 消费端（shell 线程）

```cpp
bool Log::FormatOnePendingRecord()
{
    unsigned key = irq_lock();
    LogRecord* r = recq_.PopNextRecordLocked(dbg_.ActiveEpoch());   // 作废记录跳过回收
    irq_unlock(key);
    if (r == nullptr) return false;

    char text[kLogBufSize];
    FormatRecordToText(*r, text, sizeof(text));

    const LogChannelPolicy& policy = GetLogChannelPolicy(r->channel);
    char out[kLogBufSize + kColorOutExtra];
    int n = WrapOutputFrame(policy.prefix, r->ansi, r->color, text, out, sizeof(out));
    if (n > 0) QueueFrameForShellSend(out, n, r->prio, r->epoch);

    key = irq_lock();
    recq_.ReleaseRecordLocked(r);        // 格式化后才归还，避免被 ISR 提前复用
    irq_unlock(key);
    return true;
}
```

### 3.9 并发保护

队列共享状态被**任务上下文**（各 `PrintXxx` / `SendCommandLine` / `PrintSelectedDebug` / `QueueLogRecord`）与 **ISR 上下文**（`HandleTxDone` / ISR 中的 `QueueLogRecord`）同时访问：

| 入口 | 锁 | 说明 |
|------|----|------|
| `QueueLogRecord` | `irq_lock` | LogRecord 入队锁内 |
| `QueueFrameAndSendNow` | `irq_lock` | 入帧池 + `PumpTxQueue` 提交都在锁内 |
| `QueueFrameForShellSend` | `irq_lock` | 入帧池 + give 锁内 |
| `SendNextFrameIfIdle` | `irq_lock` | `PumpTxQueue` 锁内 |
| `HandleTxDone` | `irq_lock`（Transport 内） | 清 `sending_`；give 在锁外 |
| `FormatOnePendingRecord` | `irq_lock`（出队/归还） | 格式化在锁外；记录格式化后才归还 |
| `Select` / `Deselect` | 无（只动注册表） | 作废靠出队 epoch 比对，不再遍历队列 |

加锁点集中在**入口**，内部 helper（`*Locked` / `PumpTxQueue`）只在锁内被调用，`FormatRecordToText` / `WrapOutputFrame` 不持锁。

### 3.10 初始化生命周期

```
shell thread_init（[shell.cpp](shell.cpp)，PreInit 阶段）
  UartDma::Config cfg; cfg.base_cfg.tx_cb = Log::HandleTxDone;
  rx.Init(DEVICE_DT_GET(DT_ALIAS(shell_uart)), cfg)   ← shell-uart = uart3
  shell_.Init(rx)                        ← Stream& 绑定
  Log::Init()                            ← 清选中/计数/代数 + 清发送中 + 重建空闲链
  Log::BindOutputStream(&rx)             ← 绑定发送通道（pump_.Bind）
  DUST_LOG_INF("shell init")

shell thread_start（PreThread 阶段）
  shell_.Start()
  Log::MarkShellThreadReady()            ← 置 shell_own_=true，切异步段
  DUST_LOG_INF("shell send owner taken")
```

`Log::Init()` 之后任何上下文（含 main 的 System_Startup、ISR）都能安全调用 DUST_LOG_*。

### 3.11 log 命令

```
log list                遍历条目，输出 "名字 [ON]/[off] [vofa]/[color]"
log on <name>           Select → "log on: ok" / "log on: not found"
log off                 Deselect → "log off: ok"
log mode <name> on|off  设置输出模式 → "log mode: vofa" / "log mode: color" / "log mode: not found"
                        （on/vofa 同义 = 纯文本；off/color 同义 = 上色）
log <其他>              "?: log list|on <name>|off|mode <name> on|off"
```

`log mode` 的名字必须已注册（`DUST_LOG_DBG("name", ...)` 出现过），否则回 `not found`，不创建幽灵条目。

---

## 4. var 模块详解

### 4.1 类型系统（[var.hpp](var/var.hpp)）

```cpp
enum class VarType : uint8_t { Uint8, Int8, Uint16, Int16, Uint32, Int32, Uint64, Int64, Float, Double, Bool };

union VarValue        // 任意类型变量的值容器（REGISTER_SHELL_VAR 的 static_assert 保证放得下）
{ uint8_t u8; int8_t i8; uint16_t u16; int16_t i16; uint32_t u32; int32_t i32;
  uint64_t u64; int64_t i64; float f; double d; bool b; };

struct Entry          // 链接段条目
{
    const char *name;   // 变量名称（var 命令用）
    VarType     type;   // 变量类型（TypeMap 自动推导）
    void       *ptr;    // 变量指针（读写目标）
};
```

`TypeMap` 用 `decltype(var_)` 推导类型，`TypeMap<T&>` 剥引用——**文件级变量、类成员变量、引用**都能正确映射。

### 4.2 注册宏

```cpp
#define REGISTER_SHELL_VAR(name_, var_)                                                   \
    static ::debug::Entry PP_CONCAT(s_shell_var_, __COUNTER__)                            \
        __attribute__((used, section(".shell_var"), aligned(sizeof(void*)))) = {          \
        .name = (name_),                                                                  \
        .type = ::debug::TypeMap<decltype(var_)>::type,                                   \
        .ptr  = static_cast<void*>(&(var_)),                                              \
    };                                                                                    \
    static_assert(sizeof(var_) <= sizeof(::debug::VarValue), "too large")
```

- 编译期在 `.shell_var` 链接段生成 Entry（`__COUNTER__` 唯一名），链接器收集边界，`var` 命令遍历。
- `static_assert` 防止注册超大类型。
- `CONFIG_DUST_CMD_SHELL_VAR=n` 时宏为空。

### 4.3 命令实现（[var.cpp](var/var.cpp)）

| 函数 | 行为 |
|------|------|
| `Find(name)` | 遍历 `.shell_var` 段按名字匹配 |
| `CmdList()` | 遍历段输出 `名字 (类型) = 值` + `--- N variables ---` |
| `CmdGet(name)` | 输出 `名字 = 值`；找不到 `not found: <name>` |
| `CmdSet(name, val)` | 按类型解析值字符串并写入 |
| `PrintVar` / `PrintValueOnly` | 11 种类型分支打印（float/double 转 double 后 `%f`） |

**数值解析**（`CmdSet`）：无符号 `strtoull`（支持 0x），有符号 `strtoll`，浮点 `strtof/strtod`，布尔 `true/1|false/0`；`*end != '\0'` → `bad value`。整数类型**不做范围检查**（`var set t_u8 300` 写入 300&0xFF=44）。

所有输出走 `Log::SendCommandLine`（Cmd 档）。

---

## 5. shell 底座详解

### 5.1 Shell 类（[shell.hpp](shell.hpp)）

```cpp
class Shell final
{
public:
    bool Init(Stream &stream);
    void Start(ThreadPrio prio = ThreadPrio::Lowest)
    { thread_.Start(TaskEntry, prio, this, "shell"); }
private:
    static constexpr size_t kLineBufSize = 128;
    Stream      *stream_   = nullptr;   // Stream 抽象（不绑 UartDma）
    uint8_t      line_buf_[kLineBufSize];
    uint32_t     line_pos_ = 0;
    Thread<2048> thread_   {};
};
```

### 5.2 线程主循环（打印 + 接收，[shell.cpp](shell.cpp) `Shell::Task`）

```cpp
void Shell::Task()
{
    for (;;)
    {
        k_sem_take(&stream_->sem_, K_FOREVER);   // 通道事件：接收数据 OR 日志发送需要驱动

        Log::SendNextFrameIfIdle();               // 先驱动日志发送（DMA 空闲则续发）

        while (Log::FormatOnePendingRecord())     // 出队参数快照请求 → 展开入帧池 → 泵
        {
            Log::SendNextFrameIfIdle();
        }

        uint8_t buf[32];
        uint16_t n = stream_->Read(buf, sizeof(buf));
        if (n == 0) continue;                     // 发送唤醒，无接收数据

        for (uint16_t i = 0; i < n; i++)          // 逐字符处理
        {
            uint8_t ch = buf[i];
            if (ch == '\r' || ch == '\n') { line_buf_[line_pos_] = 0; if (line_pos_>0) ProcessLine(line_buf_); line_pos_ = 0; }
            else if (ch == '\b' || ch == 0x7F) { if (line_pos_ > 0) line_pos_--; }
            else if (line_pos_ < kLineBufSize - 1) line_buf_[line_pos_++] = ch;
        }
    }
}
```

**线程是日志发送的驱动 + 接收处理**：
1. `sem_` 从"接收通知信号量"升级为**"通道事件信号量"**（接收数据 OR 日志发送需要驱动）。
2. 唤醒后**先 `SendNextFrameIfIdle()`** 泵日志帧，再通过 **`FormatOnePendingRecord()`** 展开异步段参数快照，最后才 `Read` 接收数据。
3. `sem_` 是 limit=1 计数信号量，接收/发送一起 give 会合并成一次唤醒——但安全：一次唤醒顺序执行全部处理，**数据不依赖 sem_ 计数**（接收数据在 Stream 缓冲、日志帧在队列，sem_ 只是"醒来处理"通知）。
4. **前提**：shell 线程是 `stream_` 的**唯一消费者**（remote/gimbal 各自持有自己的 Stream 实例，不共享）。

### 5.3 命令分派

`Shell::ProcessLine` 只做两件事：取第一个 token，然后在 `.shell_cmd` 段里线性匹配。
具体有哪些命令族、每个族怎么解析参数，都不在本文件里——详见 [第 6 章](#6-命令族注册与分派)。

```cpp
void Shell::ProcessLine(uint8_t *line)
{
    const char* cmd = nullptr;
    uint8_t* rest   = NextToken(line, cmd);
    if (*cmd == '\0') return;

    for (const ShellCmdEntry* e = ShellCmdBegin(); e < ShellCmdEnd(); ++e)
    {
        if (std::strcmp(e->name, cmd) == 0) { e->handler(rest); return; }
    }
    Log::SendCommandLine("unknown command");
}
```

帮助 `h` 同样遍历该段输出，所以**新增命令族后帮助自动出现**。

### 5.4 初始化与注册

```
REGISTER_INIT  (thread_init,  PreInit, High, HaltOnFail, nullptr, "dbg_init")  ← PreInit 初始化 uart3 + Log
REGISTER_THREAD(thread_start, PreThread, HaltOnFail, "dbg_start")              ← PreThread 启动 shell 线程
```

`thread_init` 用 `DT_ALIAS(shell_uart)`（overlay 配 `shell-uart = &uart3`），注册 `tx_cb = Log::HandleTxDone` → `Log::Init()` → `Log::BindOutputStream(&rx)`。

---

## 6. 命令族注册与分派

### 6.1 为什么用链接段

传统写法要在 `shell.cpp` 里维护一张中央命令表：

```cpp
static const Cmd kCmds[] = { { "var", Var::Process }, { "log", ... } };
```

问题是「有哪些命令」分散在两处：命令实现和这张表。漏加一行，编译不报错，运行时才发现命令不存在。

链接段把「存在性」交给链接器：**实现和注册是同一行代码**，没有遗忘的可能。
项目里已有六处同构用法：`.user_init` / `.user_thread` / `.shell_var` / `.shell_cmd` / `.remote` / `.imu`。

### 6.2 数据结构（[shell_cmd.hpp](shell_cmd.hpp)）

```cpp
struct ShellCmdEntry
{
    const char* name;                  // 命令族名
    void (*handler)(uint8_t* args);    // 族入口，收到参数原始串
    const char* help;                  // 帮助摘要（一行）
};
```

### 6.3 注册宏

```cpp
REGISTER_SHELL_CMD(name_, help_, handler_, ident_)
```

| 参数 | 说明 |
|------|------|
| `name_` | 命令族名（字符串字面量） |
| `help_` | 帮助摘要（字符串字面量，一行） |
| `handler_` | 族入口，签名 `void(uint8_t* args)` |
| `ident_` | 注册项标识（唯一标识符，用于生成变量名，通常与命令族同名） |

展开为：

```cpp
static const ::debug::ShellCmdEntry kShellCmd_<ident_>
    __attribute__((used, __section__(".shell_cmd"))) = { name_, handler_, help_ };
```

**为什么需要 `ident_`**：`handler_` 可能是 `ThreadCmd::Process` 这种带 `::` 的限定名，
不能直接拼进标识符（`kShellCmd_ThreadCmd::Process` 非法）。用独立的 `ident_` 解耦命名。

**代价**：每个命令族占 12 B（三个指针）。当前 5 个族 = 60 B。

### 6.4 链接段

`framework/cmd/linker/dust_init.ld`：

```ld
SECTION_PROLOGUE(.shell_cmd,,)
{
    __shell_cmd_start = .;
    KEEP(*(.shell_cmd))
    __shell_cmd_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)
```

**只读段**（`ROM_SECTIONS`）——命令名和函数指针启动后不变。

> 注意：段边界符号是 **C 链接符号**。`shell_cmd.hpp` 里用 `extern "C"` 声明，
> 否则 C++ 名字修饰会让链接器找不到（会报 `undefined reference to debug::__shell_cmd_start`）。

### 6.5 分派流程

```text
Shell::ProcessLine(line)
  ├── NextToken(line, cmd)            ← 取第一个 token
  ├── cmd 为空 → return
  ├── 遍历 ShellCmdBegin() .. ShellCmdEnd()
  │     └── strcmp(entry->name, cmd) 匹配
  │           └── entry->handler(rest)  ← 剩余参数串原样交给族
  └── 未匹配 → "unknown command"
```

**各族的 handler 收到的是「命令名之后」的原始串**，参数怎么切由族自己决定：

```cpp
void Var::Process(uint8_t *line)
{
    const char* sub = nullptr;
    line = NextToken(line, sub);       // 取子命令

    if (std::strcmp(sub, "get") == 0) {
        CmdGet(reinterpret_cast<const char*>(line));
    }
    ...
}
```

### 6.6 帮助自动生成

`h` 命令本身也是一个注册项：

```cpp
static void HelpCmd(uint8_t* args) { (void)args; Shell::PrintHelp(); }
REGISTER_SHELL_CMD("h", "help", HelpCmd, help);
```

`Shell::PrintHelp()` 遍历同一个段：

```cpp
void Shell::PrintHelp()
{
    for (const ShellCmdEntry* e = ShellCmdBegin(); e < ShellCmdEnd(); ++e)
    {
        char line[80];
        snprintf(line, sizeof(line), "%-8s %s", e->name, e->help);
        Log::SendCommandLine(line);
    }
}
```

输出样例：

```text
h        help
boot     boot timing
thread   thread list|show|pause|resume|reset
var      var list|get|set
log      log list|on|off|mode
```

**顺序由链接器决定**，不是注册的书写顺序。当前 5 条命令怎么排都看得清，
若将来需要固定顺序，可给 `ShellCmdEntry` 加 `order` 字段并排序输出。

### 6.7 行切分工具（[token.hpp](token.hpp)）

```cpp
inline uint8_t* SkipSpace(uint8_t* p);
inline uint8_t* NextToken(uint8_t* line, const char*& out);
```

`NextToken` 原地把 token 末尾的空格改成 `'\0'`，返回下一个 token 的起点。
连续空格按一个分隔符处理。

**所有命令族共用**，替换掉了此前 5 个文件里各自手抄的 5 行切分代码。

### 6.8 新增命令族的完整步骤

```text
① 新建 xxx_cmd.hpp / xxx_cmd.cpp（或写进已有文件）
② 实现 void XxxCmd::Process(uint8_t* line)
      - NextToken 取子命令
      - 未知子命令输出用法提示（不带 ?: 前缀）
③ 文件末尾写 REGISTER_SHELL_CMD("xxx", "xxx ...", XxxCmd::Process, xxx);
④ framework/cmd/CMakeLists.txt 的 DUST_CMD_SHELL 块加源文件（若是新 .cpp）
```

**不需要改 `shell.cpp`，不需要改帮助列表。**

---

## 7. thread 命令族

### 7.1 数据来源

[thread_cmd.cpp](thread/thread_cmd.cpp) 遍历 `thread_registry`（`framework/init/thread/`），
取每个 `ThreadBase` 的 `ThreadStats` 快照。

```cpp
class ThreadBase
{
    virtual const char* Name() const = 0;
    virtual bool        GetStats(ThreadStats& out) const = 0;
    virtual void        ResetStats() = 0;
    virtual void        Pause() = 0;
    virtual void        Resume() = 0;
    virtual bool        Paused() const = 0;
};
```

### 7.2 统计字段

| 字段 | 含义 |
|------|------|
| `run_count` | 任务体进入次数 |
| `retry_count` | 可恢复错误次数 |
| `fatal_count` | 不可恢复错误次数 |
| `drop_count` | 业务侧 `CountDrop()` 计数 |
| `run_ns` / `max_run_ns` / `last_run_ns` | 累计 / 最长 / 最近一次耗时 |
| `stack_unused` | 栈剩余字节（需 `CONFIG_INIT_STACKS` + `CONFIG_THREAD_STACK_INFO`） |
| `running` / `paused` | 状态 |

**只统计受管任务**（`Start(TaskFunc, period, ...)`）。裸任务（`Start(k_thread_entry_t, ...)`）
没有外壳，`run_count` 恒为 0，但暂停/恢复仍然有效。

### 7.3 命令

```text
thread list             每行：THREAD / STATE / RUN / RETRY / FATAL / DROP / STACK
thread show <name>      state / run / retry-fatal / drop / last / max / total / stack
thread pause <name>     k_thread_suspend，保留全部现场
thread resume <name>    k_thread_resume，从挂起处继续
thread reset <name>     清零计数器（不影响暂停状态）
```

**状态取值**：`running` / `paused` / `dead`。

`pause` / `resume` 是**内核级挂起**——被暂停的线程若持有互斥量，等待它的线程会永久阻塞。
当前工程只用 `irq_lock` 短临界区，暂停是安全的。

### 7.4 使用注意

- 名字找不到 → `thread xxx: not found`
- 缺参数 → `usage: thread xxx <name>`
- `thread list` 只显示**真正启动过**的线程（`Thread::Start` 时登记）

---

## 8. boot 命令族

### 8.1 口径

`boot timing` 输出的是**每个 init / thread 项的执行耗时**，不含阶段间 `k_msleep(500)`。

### 8.2 数据来源

`REGISTER_INIT` / `REGISTER_THREAD` 每次展开都会额外生成一个 `BootTimingSlot`：

```cpp
struct BootTimingSlot
{
    const char* name;   // 项目名
    uint32_t    ns;     // 耗时（启动器写入）
    bool        ok;     // 是否成功（启动器写入）
};
```

放进 `.boot_timing` 段（`framework/cmd/linker/boot_timing.ld`，**DATA_SECTIONS 可写**）。
槽数量由链接器按实际注册项决定，没有固定容量。

### 8.3 输出

```text
boot timing
BOOT TIMING (total 42 ms, 8 items)
  imu_init          25.134 ms   ok
  w25q128            8.021 ms   ok
  dbg_init           3.552 ms   ok
  ...
```

按耗时降序；失败项标 `FAIL`；无记录时输出 `(no boot timing recorded)`。

### 8.4 用途

- 定位「哪个 init 拖慢了启动」
- 对比改动前后的启动耗时
- 配合 `[boot] xxx fail` 日志判断失败发生在哪一项

---

## 9. 命令参考

| 命令 | 输出 |
|------|------|
| `h` | 遍历 `.shell_cmd` 段，每族一行（顺序由链接器决定） |
| `var list` | 每个变量一行 `名字 (类型) = 值` + `--- N variables ---` |
| `var get <name>` | `名字 = 值`；`not found: <name>` |
| `var set <name> <val>` | `ok`；坏值 `bad value`；缺值 `usage: var set <name> <value>` |
| `log list` | 每个 DBG 条目一行 `名字 [ON]/[off] [vofa]/[color]` |
| `log on <name>` | `log on: ok`；不存在 `log on: not found` |
| `log off` | `log off: ok` |
| `log mode <name> on` | `log mode: vofa`；不存在 `log mode: not found`；缺参 `log mode <name> on\|off` |
| `log mode <name> off` | `log mode: color`；不存在 `log mode: not found` |
| `thread list` | 每个线程一行 `名字 状态 RUN RETRY FATAL DROP STACK` + `--- N threads ---` |
| `thread show <name>` | 多行：state / run / retry-fatal / drop / last / max / total / stack |
| `thread pause <name>` | `thread pause: ok`；不存在 `thread pause: not found` |
| `thread resume <name>` | `thread resume: ok`；不存在 `thread resume: not found` |
| `thread reset <name>` | `thread reset: ok`；不存在 `thread reset: not found` |
| `boot timing` | `BOOT TIMING (total N ms, M items)` + 按耗时降序的项列表；无记录 `(no boot timing recorded)` |
| 未知命令 | `unknown command` |

---

## 10. 容量模型与实测数据（2026-08-06，921600 波特）

**工程公式**（帧池 8 帧）：

```
B_max = K + 1               同一时间允许的最大日志调用数（1 DMA 中 + K 排队）
K_min = max(1, B - 1)       给定突发需求 B 所需最少帧数
不丢帧 ⇔ R_p < baud/10 且 B ≤ K + 1    （稳态吞吐 + 瞬时突发）
```

| 项 | 实测值 |
|----|--------|
| 单帧周期（30/64/127B） | 371 / 743 / 1430us（理论 330/694/1378），回归 T = 38.2 + 10.96×len |
| 固定开销/帧 | ~40us（任务侧 Send/take 调度 + ISR + TEMT 余量） |
| 瞬时容量（8 帧池） | 最多 9 条同时发送，第 10 条起丢（事件永不挤） |
| 无限闭环 | 稳态 <92160 B/s（921600 波特 / 10bit）时零丢帧；满速（>92KB/s）丢帧 60% = 容量边界，扩帧不可救只能限速 |
| 调用点耗时（相同字符，2026-08-10 实测） | 同步段直发 96us → 异步段线程参数快照 9us（省 ~87us，~10x） |

**异步段新增容量**：调用点只做参数快照入队（LogRecord 8 条 FIFO），不占帧池——帧池容量留给格式化后待发的帧。LogRecord 池满直接丢（O(1)），不挤帧。

**工程建议**：日志内容 <98B（127B 上限含 ~29B ANSI 开销）；瞬时突发 ≤9 条；持续输出限速 <92KB/s。

---

## 11. FAQ

**Q：DUST_LOG 打印依赖线程吗？**
A：**分时段**。boot 早期（`shell_own_=false`）调用点直发，不依赖线程；shell 线程启动（`PreThread`，`MarkShellThreadReady`）接管后走异步，依赖 shell 线程 `FormatOnePendingRecord()` 展开快照 + `SendNextFrameIfIdle()` 驱动 DMA。唯一前置是 `Log::Init()` / `Log::BindOutputStream()` 已执行（`dbg_init`，PreInit 阶段）。

**Q：异步段调用点（ISR）会做什么？**
A：`BuildRecordFromVaList`（扫 fmt + va_arg，~µs 级）+ `QueueLogRecord`（短临界区入队）+ give。**无 vsnprintf、无 DMA 提交、无链表遍历挤帧**。

**Q：为什么线程里不用 vsnprintf，要 FormatRecordToText 逐段拼？**
A：vsnprintf 需要 `va_list`，而 `va_list` 是栈指针、调用点函数返回即失效，传不到线程。快照存的是**参数值**，`FormatRecordToText` 只能按槽位约定用 snprintf 逐段展开。

**Q：帧池为什么是 8 帧？会不会不够？**
A：8×128B=1KB。帧即取即还（Send 提交即回收），稳态限速下不被 DMA 长时间占住；瞬时突发上限 = 帧数+1 = 9 条，第 10 条起丢（事件永不挤，DBG 先被挤）。

**Q：参数快照的 `%s` 有坑吗？**
A：有。快照存指针，延迟格式化要求指针仍有效——**只许字符串字面量**。临时栈 `char buf[]` 传入会读野指针（最高风险）。项目盘点 + review 把关。

**Q：boot 早期日志为什么是直发？**
A：shell 线程 `PreThread` 才启动，boot 早期若只入队会无人消费，帧积压满后 Early/Mid/Late 阶段日志全被吞（阶段2 回归诊断的教训）。同步段调用点直发保证即时出、不积压。

**Q：DBG 默认是什么形态？怎么切 VOFA+？**
A：默认**上色**（薄荷绿 ANSI），终端可读。要喂 VOFA+ FireWater 绘图，先 `log mode <name> on` 把该名字切成纯文本（切回用 `off`）。模式按名字独立，`log list` 每行末尾 `[vofa]/[color]` 显示当前模式。

**Q：为什么 VOFA+ 模式必须关掉 ANSI？**
A：ANSI 转义串 `\x1b[38;2;R;G;Bm` 里的 `38/2/123/247/205` 这些数字会被 VOFA+ 当成通道数据解析，曲线全乱。纯文本模式只输出 `v0,v1\r\n`。

**Q：`log on/off` 和 `log mode` 有什么区别？**
A：`log on <name>` 决定**打不打**（选中/静默），`log mode <name> on|off` 决定**打成什么形态**（上色/纯文本）。两者独立，同一时间只选中一条，但每条各自记着自己的模式。

**Q：切换 `log on` 时队列里残留的旧 DBG 帧怎么处理？**
A：靠**选择代数（epoch）**。注册表每次 Select/Deselect 递增 `epoch_`，DBG 记录/帧携带创建时的 epoch，出队时与当前值不一致就回收丢弃。不需要遍历队列打作废标记；代价是最多残留一帧「已在 DMA 中」的旧帧（无法取消，可接受）。

**Q：新增一个日志优先级/通道要改几处？**
A：三处表驱动，不碰逻辑：①`LogChannel`/`TxPriority` 加枚举；②`kLogChannelPolicies` 加一行（颜色/前缀/ANSI/优先级）；③必要时 `kTxPriorityPolicies` 加一行（池满挤出策略）。`EvictLowestPriorityFrameLocked` / `WrapOutputFrame` 等实现不用动。

**Q：为什么 `log on` 不存在的名字回 not found？**
A：`Select` 只选中**已存在**条目（`DUST_LOG_DBG("name",...)` 注册过的）。用 FindOrCreate 会凭空创建幽灵条目——已修复。`log mode` 同理。

**Q：`var set t_u8 300` 为什么是 ok？**
A：整数类型不做范围检查（写入 300&0xFF=44），只保证解析正确不崩溃。范围校验不属于 var 命令职责。

**Q：命令响应会不会被日志挤掉？**
A：命令响应走 Cmd 档（事件后、DBG 前），DBG 流式打印期间命令响应**先于**后续 DBG 帧显示；事件（INF/ERR/OK/WRN）永不丢。

**Q：`sem_` 是 limit=1，接收和发送一起 give 会不会丢事件？**
A：不会。合并成一次唤醒但顺序执行 `SendNextFrameIfIdle`+`FormatOnePendingRecord`+`Read`，发送与接收都被处理；数据在队列/缓冲里不依赖 sem_ 计数，最多延迟到下次处理。

**Q：新增一个命令族要改几个地方？**
A：**只改自己的文件**。写 `void Xxx::Process(uint8_t*)` + 一行 `REGISTER_SHELL_CMD(...)`，若是新 `.cpp` 再在 CMake 里加源文件。`shell.cpp` 和帮助列表都不用动——`h` 是遍历 `.shell_cmd` 段生成的。

**Q：`h` 的输出顺序为什么每次可能不同？**
A：`.shell_cmd` 段内顺序由链接器决定，不保证等于注册的书写顺序。当前 5 条命令怎么排都看得清；若需要固定顺序，给 `ShellCmdEntry` 加 `order` 字段并排序输出。

**Q：为什么 `REGISTER_SHELL_CMD` 要四个参数而不是三个？**
A：第四个 `ident_` 用来生成变量名。`handler_` 可能是 `ThreadCmd::Process` 这种带 `::` 的限定名，不能拼进标识符（`kShellCmd_ThreadCmd::Process` 非法）。拆开后 `nm` 里也能看到 `kShellCmd_thread` 这种人能读的名字。

**Q：段边界符号链接时报 `undefined reference to debug::__shell_cmd_start`？**
A：声明写进了 `namespace debug`，C++ 名字修饰让它变成 `debug::__shell_cmd_start`，而链接脚本导出的是 C 符号 `__shell_cmd_start`。用 `extern "C"` 声明即可。

**Q：为什么 `.shell_cmd` 用 ROM_SECTIONS，而 `.boot_timing` 用 DATA_SECTIONS？**
A：命令表只存名字和函数指针，启动后不变，只读即可；耗时槽需要在启动时写入 `ns` / `ok`，必须是可写的 RAM 段（初值从 ROM 拷贝）。

**Q：`thread list` 里的 RUN 为什么一直是 0？**
A：该线程用的是裸任务（`Start(k_thread_entry_t, ...)`），没有受管任务外壳，不采集统计。要统计就改成 `Start(TaskFunc, period, ...)` 并把任务体写成「一拍」。

**Q：`thread pause` 之后 `thread resume` 没反应？**
A：先看 `thread list` 的 STATE：如果是 `dead`，线程已经退出了（`Fatal` 或钩子返回 false），`resume` 对已退出的线程无效。`paused` 状态才可恢复。

**Q：`boot timing` 的 total 为什么比实际启动时间小很多？**
A：`total` 是**所有条目执行时间之和**，不含阶段间的 `k_msleep(500)`（9 次 = 4.5 秒）。要看总启动时间需要另外打点。

**Q：CONFIG_DUST_CMD_SHELL_LOG 关闭时调用 DUST_LOG_* 会怎样？**
A：宏为空（log.hpp `#else` 空宏），调用点零开销、编译不报错——与 Zephyr LOG=n 静默语义一致。
