# shell 调试台模块（log / var / shell 底座）

> 一套自研的 DMA 日志系统（DUST_LOG）与自维护 UART shell 融合的统一调试台。
> 三模块分工：**log**（日志打印 + 帧队列仲裁）、**var**（调试变量注册与读写）、**shell**（UART 线程底座 + 命令分发）。
> 替代 Zephyr log，根治 uart3 回调槽冲突卡死。

---

## 目录

1. [模块总览](#1-模块总览)
2. [快速上手](#2-快速上手)
3. [log 模块详解](#3-log-模块详解)
4. [var 模块详解](#4-var-模块详解)
5. [shell 底座详解](#5-shell-底座详解)
6. [命令参考](#6-命令参考)
7. [容量模型与实测数据](#7-容量模型与实测数据)
8. [FAQ](#8-faq)

---

## 1. 模块总览

| 模块 | 文件 | 职责 | Kconfig 符号 |
|------|------|------|-------------|
| log | [log.hpp](log.hpp) / [log.cpp](log.cpp) | Log 类 + DUST_LOG 宏族 + 帧池三档优先级发送仲裁 | `DUST_CMD_SHELL_LOG` |
| var | [var.hpp](var.hpp) / [var.cpp](var.cpp) | 调试变量注册（`.shell_var` 链接段收集）+ var list/get/set | `DUST_CMD_SHELL_VAR` |
| shell | [shell.hpp](shell.hpp) / [shell.cpp](shell.cpp) | UART 线程底座（接收循环 + 命令分发）+ 初始化接入 | `DUST_CMD_SHELL` |

**依赖链**：

```
USE_CMD_VAR → DUST_CMD_SHELL_VAR ─┐
                                  ├→ DUST_CMD_SHELL → DUST_COM_UART_DMA
USE_CMD_LOG → DUST_CMD_SHELL_LOG ─┘
```

- `DUST_CMD_SHELL` 是公共底座（线程 + 分发），`DUST_CMD_SHELL_VAR` / `DUST_CMD_SHELL_LOG` 各自 select 它
- `log` 依赖 `shell`：`Log::Init()` / `Log::BindUart()` 在 shell 的 `dbg_init` 里执行——没有 shell 就没人初始化 Log（`uart_ == nullptr` 时发送静默丢弃）；`log` 命令也走 shell 的 `ProcessLine` 分发

**关键设计**：发送路径唯一——log 与 shell 共用 `UartDma` 的 DMA 发送通道（[shell.cpp:139](shell.cpp#L139) `tx_cb = Log::OnTxDone`），不碰 Zephyr `uart_callback_set` 槽位，天然无冲突。

---

## 2. 快速上手

### 打日志

```cpp
#include "log.hpp"

DUST_LOG_INF("vx=%.2f", vx);              // 黑色 [inf]，一次性
DUST_LOG_ERR("can tx fail %d", ret);      // 亮红 [err]，一次性
DUST_LOG_OK("power in budget");           // 亮绿 [ok]，一次性
DUST_LOG_WRN("imu drift");                // 亮黄 [wrn]，一次性
DUST_LOG_DBG("test_vx", "vx=%.2f", vx);   // 白色，带名字——默认静默，log on test_vx 后流式打印
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
h                帮助
var list         列出所有调试变量
var get <name>   查看变量
var set <name> <val>  修改变量
log list         列出所有 DBG 日志条目
log on <name>    选中一条日志流式打印（同一时间只打一条）
log off          停止流式打印
```

---

## 3. log 模块详解

### 3.1 枚举与常量（[log.hpp:26-53](log.hpp#L26-L53)）

```cpp
enum class LogColor : uint8_t
{
    Black  = 30,   // \x1b[30m（INF）黑
    Red    = 91,   // \x1b[91m（ERR）亮红
    Green  = 92,   // \x1b[92m（OK）亮绿
    Orange = 93,   // \x1b[93m（WRN）亮黄
    White  = 37,   // \x1b[37m（DBG）白
};

enum class TxPriority : uint8_t
{
    Event = 0,   // INF/ERR/OK/WRN：最高，插队头，永不挤
    Cmd   = 1,   // 命令响应（var/log 输出）：中，插事件后、DBG 前
    Dbg   = 2,   // DBG 流式：最低，排队尾，先被挤
};
```

| 常量 | 值 | 含义 |
|------|-----|------|
| `kTxFrameSize` | 128 | 帧数据区大小（含尾部 `\0` 保险） |
| `kTxMaxLen` | 127 | 发送最长字节长度，超长截断 |
| `kTxPoolCount` | 4 | 帧池帧数（4 × 128B = 512B） |
| `kMaxLogEntries` | 64 | DBG 条目数上限 |
| `kNullIndex` | 255 | 帧索引链表空值 |

**每帧字节开销**（宏层拼接，[log.hpp:139-140](log.hpp#L139-L140)）：

```
16B = [inf]前缀(5) + 颜色 \x1b[91m(5) + 复位 \x1b[0m(4) + \r\n(2)
帧上限 127B → 内容建议 ≤111B（DBG 无前缀，固定开销 11B，内容 ≤116B）
```

### 3.2 数据结构（[log.hpp:58-73](log.hpp#L58-L73)）

```cpp
struct TxFrame        // 发送帧（三档共用）
{
    char       data[kTxFrameSize];   // 格式化后内容（含 ANSI 颜色），超 kTxMaxLen 截断
    uint16_t   len;                  // 实际有效长度（≤127）
    TxPriority prio;                 // 优先级档位
    bool       is_stale;             // DBG 切换时标记作废（及时顶替用）
    uint8_t    next;                 // 帧索引链表（255=nullptr）
};

struct LogEntry       // DBG 日志条目（只服务 DBG）
{
    const char* name;   // DBG 名字（log on 用，运行时 FindOrCreate 创建）
};
```

帧池是**静态数组** `tx_pool_[4]` + 两条索引链表：**空闲链表**（`free_head_`）和**发送队列**（`tx_head_/tx_tail_`）——帧在两条链之间流转，零动态分配。

### 3.3 DBG 条目机制（无 DEFINE、无链接段、无链表）

DBG 条目是**运行时注册**的：首次调用 `DUST_LOG_DBG("name", ...)` 时由 `FindOrCreate` 创建入静态数组 `entries_[64]`（名字即标识，同名复用同一条目）。

| 函数 | 行为 |
|------|------|
| `FindOrCreate(name)`（[log.cpp:59-71](log.cpp#L59-L71)） | 遍历数组按名字查找；找不到且池未满 → 创建；池满返回 nullptr（该条静默） |
| `Select(name)`（[log.cpp:84-96](log.cpp#L84-L96)） | **只选中已存在条目**——遍历查找，存在则 `active_` 指向它并 `MarkStaleDbg()`（队列中残留旧 Dbg 帧作废）；不存在返回 false（`log on` 回 not found，不创建幽灵条目） |
| `Deselect()`（[log.cpp:101-105](log.cpp#L101-L105)） | `active_ = nullptr` + `MarkStaleDbg()` |
| `Active()` / `First()` / `Next()` | 遍历数组用（`log list`）：`First` 返回 `entries_[0]`，`Next` 用指针差算索引 |

**选中状态 = 单个 `active_` 指针**：`log on` 改指针指向选中条目（旧选中自动被顶替），**同一时间只打一条 DBG**。`Dbgl` 仅当 `e == active_` 才发（未选中直接 return，零开销）。

### 3.4 打印入口

| 函数 | 路径 |
|------|------|
| `Inf/Err/Ok/Wrn`（[log.cpp:161-203](log.cpp#L161-L203)） | `PrintColor(color, fmt, ap)` → 格式化 + `\x1b[3Xm...\x1b[0m\r\n` 上色 → `TrySend(..., TxPriority::Event)` |
| `Dbgl`（[log.cpp:141-155](log.cpp#L141-L155)） | `e != active_` 静默；否则格式化 + 白色上色 → `TrySend(..., TxPriority::Dbg)` |
| `SendLine`（[log.cpp:225-230](log.cpp#L225-L230)） | 命令响应直发（不经过 log 过滤，带 `\r\n`）→ `TrySend(..., TxPriority::Cmd)` |

所有打印最终都汇入 `TrySend`——**发送仲裁的唯一入口**。

### 3.5 发送仲裁（核心）

#### TrySend：入队仲裁（[log.cpp:397-423](log.cpp#L397-L423)）

```
1. len > kTxMaxLen → 截断到 127
2. irq_lock() 开始（并发保护）
3. AllocFrame() 从空闲池取帧
4. 池空 → EvictLowest() 挤最低优先级帧（DBG 先、命令次、事件永不挤）
   挤不出（全事件帧，极端）→ 丢弃本轮
5. memcpy 内容 → 设 len/prio/is_stale=false → EnqueueByPrio() 按档插队
6. DMA 空闲（!sending_）→ Dequeue() + SendFrame() 立即启动发送链
7. irq_unlock()
```

#### EnqueueByPrio：按档插队（[log.cpp:305-318](log.cpp#L305-L318)）

找第一个 `prio > 新帧 prio` 的位置插入——Event 插队头、Cmd 插事件后/DBG 前、Dbg 排队尾；**同档先来后到**（`prio <= f->prio` 的跳过）。

#### EvictLowest：池满挤帧（[log.cpp:324-347](log.cpp#L324-L347)）

遍历队列找 prio 值**最大**（最低优先）的非事件帧作为 victim，从链表中摘除返回——被新帧复用。

#### 发送链（SendFrame / OnTxDone）

```
SendFrame(f)（[log.cpp:254-262](log.cpp#L254-L262)）
  sending_ = true
  uart_->Send(f->data, f->len)     ← UartDma::Send：memcpy 到自身发送缓冲 + uart_tx 提交 DMA
  RecycleFrame(f)                  ← 无论成败立即归还空闲池！
```

**关键点：帧即取即还**——`UartDma::Send` 内部把帧内容拷进它自己的 `tx_data_`（[uart.cpp:196](E:/Zephyr/zephyr_user/framework/drivers/communication/stream/uart/uart.cpp#L196)），DMA 搬运的是 UartDma 缓冲而非 `TxFrame::data`。所以 Send 提交成功即帧使命结束，立即回空闲池。**帧池永不枯竭**（曾因发送成功不回收导致帧泄漏、池耗尽后全丢——已修复）。

```
OnTxDone()（[log.cpp:237-244](log.cpp#L237-L244)）——TX_DONE 中断回调（ISR 上下文）
  irq_lock()
  sending_ = false
  Dequeue() 取队头下一帧（is_stale 作废帧跳过回收）
  有帧 → SendFrame() 继续发送（链式驱动）
  irq_unlock()
```

**发送管线不依赖线程**：调用者（任务/ISR/main 初始化）直接 `TrySend` 入队，TX_DONE **中断**回调驱动续发。

#### Dequeue：队列弹头（[log.cpp:353-370](log.cpp#L353-L370)）

`is_stale` 的帧跳过并回收（DBG 切换及时顶替：`log on B` 时队列中残留的 A 帧作废，B 立即接管）。

#### 并发保护

队列状态（`tx_head_/tx_tail_/free_head_/sending_`）被**任务上下文**（TrySend/SendLine/Select/Deselect）和 **ISR 上下文**（OnTxDone）同时访问——所有队列写操作用 `irq_lock()/irq_unlock()` 包裹，加锁点在 **TrySend / OnTxDone / MarkStaleDbg** 三个入口，内部 helper（AllocFrame/RecycleFrame/EnqueueByPrio/EvictLowest/Dequeue/SendFrame）只在锁内被调用。

### 3.6 初始化生命周期

```
shell thread_init（[shell.cpp:133-152](shell.cpp#L133-L152)，PreInit 阶段）
  UartDma::Config cfg; cfg.base_cfg.tx_cb = Log::OnTxDone;
  rx.Init(DEVICE_DT_GET(DT_ALIAS(shell_uart)), cfg)   ← shell-uart = uart3
  shell_.Init(rx)
  Log::Init()          ← 清 active_/count_，重建空闲帧链（0→1→2→3→255）
  Log::BindUart(&rx)   ← 绑定发送通道
```

`Log::Init()` 之后任何上下文（含 main 的 System_Startup、ISR）都能安全调用 DUST_LOG_*。

### 3.7 log 命令（[log.cpp:429-467](log.cpp#L429-L467)）

```
log list            遍历 entries_，输出 "名字 [ON]/[off]"
log on <name>       Select → "log on: ok" / "log on: not found"
log off             Deselect → "log off: ok"
```

---

## 4. var 模块详解

### 4.1 类型系统（[var.hpp:20-100](var.hpp#L20-L100)）

```cpp
enum class VarType : uint8_t { Uint8, Int8, Uint16, Int16, Uint32, Int32, Uint64, Int64, Float, Double, Bool };

union VarValue        // 任意类型变量的值容器（REGISTER_SHELL_VAR 的 static_assert 保证放得下）
{
    uint8_t u8; int8_t i8; uint16_t u16; int16_t i16;
    uint32_t u32; int32_t i32; uint64_t u64; int64_t i64;
    float f; double d; bool b;
};

struct Entry          // 链接段条目
{
    const char *name;   // 变量名称（var 命令用）
    VarType     type;   // 变量类型（TypeMap 自动推导）
    void       *ptr;    // 变量指针（读写目标）
};

template<typename T> struct TypeMap { static constexpr VarType type = VarType::Uint32; };
template<> struct TypeMap<uint8_t>  { static constexpr auto type = VarType::Uint8;  };
... 11 种类型全特化 + template<typename T> struct TypeMap<T&> : TypeMap<T> {};  // 引用剥壳
```

`TypeMap` 用 `decltype(var_)` 推导变量类型——**文件级变量、类成员变量（`obj.member`）、引用**都能正确映射到 VarType。

### 4.2 注册宏（[var.hpp:117-128](var.hpp#L117-L128)）

```cpp
#define REGISTER_SHELL_VAR(name_, var_)                                                   \
    static ::debug::Entry PP_CONCAT(s_shell_var_, __COUNTER__)                            \
        __attribute__((used, section(".shell_var"), aligned(sizeof(void*)))) = {          \
        .name = (name_),                                                                  \
        .type = ::debug::TypeMap<decltype(var_)>::type,                                   \
        .ptr  = static_cast<void*>(&(var_)),                                              \
    };                                                                                    \
    static_assert(sizeof(var_) <= sizeof(::debug::VarValue), ...)
```

- 编译期在 `.shell_var` 链接段生成一个 Entry（`__COUNTER__` 保证名字唯一）
- 链接段由链接器脚本收集（`__shell_var_start/end` 边界），`var` 命令遍历
- `static_assert` 防止注册超大类型（如 struct）
- `CONFIG_DUST_CMD_SHELL_VAR=n` 时宏为空（调用点零开销）

### 4.3 命令实现（[var.cpp](var.cpp)）

| 函数 | 行为 |
|------|------|
| `Find(name)`（[var.cpp:30-42](var.cpp#L30-L42)） | 遍历 `.shell_var` 段按名字匹配 |
| `CmdList()`（[var.cpp:99-113](var.cpp#L99-L113)） | 遍历段输出 `名字 (类型) = 值` + 尾部 `--- N variables ---` |
| `CmdGet(name)`（[var.cpp:119-132](var.cpp#L119-L132)） | 按名查找输出 `名字 = 值`；找不到 `not found: <name>` |
| `CmdSet(name, val)`（[var.cpp:139-219](var.cpp#L139-L219)） | 按类型解析值字符串并写入 |
| `PrintVar` / `PrintValueOnly`（[var.cpp:48-94](var.cpp#L48-L94)） | 11 种类型的分支打印（float/double 转 double 后 `%f`） |

**数值解析**（`CmdSet`）：
- 无符号：`std::strtoull(val, &end, 0)`（支持 0x 前缀），`*end != '\0'` → `bad value`
- 有符号：`std::strtoll`
- 浮点：`std::strtof` / `std::strtod`
- 布尔：`true/1` / `false/0`，其他 → `bad value`
- 注意：整数类型**不做范围检查**（`var set t_u8 300` 写入 300 & 0xFF）——只保证不崩溃，范围校验不在本模块职责

所有输出走 `Log::SendLine`（Cmd 档，不经日志过滤）。

---

## 5. shell 底座详解

### 5.1 Shell 类（[shell.hpp:31-59](shell.hpp#L31-L59)）

```cpp
class Shell final
{
public:
    bool Init(UartDma &uart);
    void Start(ThreadPrio prio = ThreadPrio::Lowest);   // thread_.Start(TaskEntry, prio, this)
private:
    static constexpr size_t kLineBufSize = 128;
    UartDma *uart_;      // 外部创建的 UartDma 实例（所有权在调用方）
    uint8_t  line_buf_[kLineBufSize];   // 行缓冲
    uint32_t line_pos_;                 // 当前行长度
    Thread<2048> thread_;               // 项目 Thread 模板
};
```

### 5.2 接收循环（[shell.cpp:84-121](shell.cpp#L84-L121)）

```
for(;;)
    k_sem_take(&uart_->sem_, K_FOREVER)   ← 阻塞等数据（DMA 回调 k_sem_give 唤醒）
    uart_->Read(buf, sizeof(buf))         ← 批量读（不逐字节轮询）
    逐字符处理：
        \r/\n → 行结束 → ProcessLine(line_buf_)（行首跳过空行）
        \b/0x7F → 退格
        其他 → 写入 line_buf_（超 127 截断）
```

### 5.3 命令分发（[shell.cpp:51-79](shell.cpp#L51-L79)）

```
解析第一个 token：
  "var" → Var::Process(line)     （var list/get/set）
  "log" → Log::Process(line)     （log list/on/off）
  "h"/"?" → CmdHelp()
  其他 → Log::SendLine("?: var/log/h")
```

### 5.4 初始化与注册（[shell.cpp:133-165](shell.cpp#L133-L165)）

```
REGISTER_INIT  (thread_init,  PreInit,    High, "dbg_init")    ← PreInit 阶段初始化 uart3 + Log
REGISTER_THREAD(thread_start, LateThread, Low,  "dbg_start")   ← LateThread 启动 shell 线程
```

`thread_init` 用 `DT_ALIAS(shell_uart)`（overlay 配 `shell-uart = &uart3`），注册 `tx_cb = Log::OnTxDone`（发送完成回调驱动帧队列续发）→ `Log::Init()` → `Log::BindUart(&rx)`。

---

## 6. 命令参考

| 命令 | 输出 |
|------|------|
| `h` / `?` | 帮助 7 行 |
| `var list` | 每个变量一行 `名字 (类型) = 值` + `--- N variables ---` |
| `var get <name>` | `名字 = 值`；`not found: <name>` |
| `var set <name> <val>` | `ok`；坏值 `bad value`；缺值 `usage: var set <name> <value>` |
| `log list` | 每个 DBG 条目一行 `名字 [ON]/[off]` |
| `log on <name>` | `log on: ok`；不存在 `log on: not found` |
| `log off` | `log off: ok`（1s 内流式停止） |

---

## 7. 容量模型与实测数据（2026-08-06，921600 波特）

**工程公式**：

```
B_max = K + 1               同一时间允许的最大日志调用数（1 DMA 中 + K 排队）
K_min = max(1, B - 1)       给定突发需求 B 所需最少帧数
不丢帧 ⇔ R_p < baud/10 且 B ≤ K + 1    （稳态吞吐 + 瞬时突发）
```

| 项 | 实测值 |
|----|--------|
| 单帧周期（30/64/127B） | 371 / 743 / 1430us（理论 330/694/1378），回归 T = 38.2 + 10.96×len |
| 固定开销/帧 | ~40us（任务侧 Send/take 调度 + ISR + TEMT 余量） |
| 瞬时容量（4 帧池） | 最多 5 条同时发送，第 6 条起丢（事件永不挤） |
| 无限闭环 | 稳态 <92160 B/s（921600 波特 / 10bit）时零丢帧；满速（>92KB/s）丢帧 60% = 容量边界，扩帧不可救只能限速 |

**工程建议**：日志内容 <116B（127B 上限含 ~11B ANSI 开销）；瞬时突发 ≤5 条；持续输出限速 <92KB/s。

---

## 8. FAQ

**Q：DUST_LOG 打印依赖线程吗？**
A：不依赖。打印是"调用者（任意上下文）入队 + TX_DONE 中断回调续发"。唯一前置是 `Log::Init()/BindUart()` 已执行（`dbg_init`，PreInit 阶段）。

**Q：帧池为什么是 4 帧？会不会不够？**
A：4×128B=512B。帧即取即还（Send 提交即回收），稳态限速下永不枯竭；瞬时突发上限 = 帧数+1 = 5 条，第 6 条起丢（事件永不挤，DBG 先被挤）。

**Q：为什么 `log on` 不存在的名字回 not found？**
A：`Select` 只选中**已存在**条目（代码里 `DUST_LOG_DBG("name",...)` 注册过的）。用 FindOrCreate 会凭空创建幽灵条目（log list 出现永不打印的条目）——已修复。

**Q：`var set t_u8 300` 为什么是 ok？**
A：整数类型不做范围检查（写入 300 & 0xFF = 44），只保证解析正确不崩溃。范围校验不属于 var 命令职责。

**Q：命令响应会不会被日志挤掉？**
A：命令响应走 Cmd 档（事件后、DBG 前），DBG 流式打印期间命令响应**先于**后续 DBG 帧显示；事件（INF/ERR/OK/WRN）永不丢。

**Q：CONFIG_DUST_CMD_SHELL_LOG 关闭时调用 DUST_LOG_* 会怎样？**
A：宏为空（log.hpp `#else` 空宏），调用点零开销、编译不报错——与 Zephyr LOG=n 静默语义一致。
