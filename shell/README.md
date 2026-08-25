# shell 调试台模块（log / var / shell 底座）

> 一套自研的 DMA 日志系统（DUST_LOG）与自维护 UART shell 融合的统一调试台。
> 三模块分工：**log**（日志打印 + 帧队列仲裁 + 参数快照异步格式化）、**var**（调试变量注册与读写）、**shell**（UART 线程底座 + 命令分发 + 日志发送驱动）。
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
| log | [log.hpp](log.hpp) / [log.cpp](log.cpp) | Log 类 + DUST_LOG 宏族 + 帧池三档优先级仲裁 + 参数快照异步格式化 | `DUST_CMD_SHELL_LOG` |
| var | [var.hpp](var.hpp) / [var.cpp](var.cpp) | 调试变量注册（`.shell_var` 链接段收集）+ var list/get/set | `DUST_CMD_SHELL_VAR` |
| shell | [shell.hpp](shell.hpp) / [shell.cpp](shell.cpp) | UART 线程底座（接收循环 + 命令分发 + 日志发送 PumpSend 驱动）+ 初始化接入 | `DUST_CMD_SHELL` |

**依赖链**：

```
USE_CMD_SHELL → select DUST_CMD_SHELL_LOG ─┐
             → select DUST_CMD_SHELL_VAR ─┴→ DUST_CMD_SHELL → DUST_COM_UART_DMA
```

- 业务层 `USE_CMD_SHELL` 是总开关：select `DUST_CMD_SHELL_LOG` + `DUST_CMD_SHELL_VAR`，二者再各自 select `DUST_CMD_SHELL`
- `DUST_CMD_SHELL` 是公共底座（线程 + 分发），`DUST_CMD_SHELL_VAR` / `DUST_CMD_SHELL_LOG` 各自 select 它
- `log` 依赖 `shell`：`Log::Init()` / `Log::BindStream()` 在 shell 的 `dbg_init` 里执行——没有 shell 就没人初始化 Log（`stream_ == nullptr` 时发送静默丢弃）；`log` 命令也走 shell 的 `ProcessLine` 分发

**关键设计（发送路径唯一）**：log 与 shell 共用同一 `Stream` 的 DMA 发送通道——shell `thread_init` 注册 `tx_cb = Log::OnTxDone`，log 发送帧经由同一个 `stream_`。不碰 Zephyr `uart_callback_set` 槽位，天然无冲突。

**通道抽象（Stream）**：shell 与 log 都持 `Stream*` 而非具体 `UartDma*`（[shell.hpp](shell.hpp) `Stream *stream_` / [log.hpp](log.hpp) `static inline Stream* stream_`）。`UartDma`、未来的 `Usb`、`RS485` 都是 `Stream` 子类——调试通道可整体替换，类型不动。

---

## 2. 快速上手

### 打日志

```cpp
#include "log.hpp"

DUST_LOG_INF("vx=%.2f", vx);              // 黑色 [inf]，一次性
DUST_LOG_ERR("can tx fail %d", ret);      // 亮红 [err]，一次性
DUST_LOG_OK("power in budget");           // 亮绿 [ok]，一次性
DUST_LOG_WRN("imu drift");                // 亮橙 [wrn]，一次性
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

### 3.1 枚举与常量

```cpp
// 颜色：TrueColor（24 位 RGB），输出 \x1b[38;2;R;G;Bm
enum class LogColor : uint32_t
{
    Black  = 0x000000,  // 黑（INF）
    Red    = 0xF50002,  // 亮红（ERR）
    Green  = 0x00F700,  // 亮绿（OK）
    Orange = 0xF6A753,  // 亮橙（WRN）
    White  = 0xFFFFFF,  // 白（DBG）
};

// 发送优先级：数值越小越靠前发
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
| `kTxPoolCount` | 8 | 帧池帧数（8 × 128B = 1KB） |
| `kMaxLogEntries` | 64 | DBG 条目数上限 |
| `kMaxLogArgs` | 4 | 参数快照槽上限（`%f` 占 2 槽） |
| `kMaxLogRecords` | 8 | LogRecord 原始请求队列深度（约 8 × 28B = 224B，32 位目标） |
| `kNullIndex` | 255 | 链表空值（数组索引，255 = 无下一项） |

**每帧字节开销**（TrueColor 宏层拼接，[log.hpp](log.hpp) 注释）：

```
[inf]前缀(5) + 颜色 \x1b[38;2;R;G;Bm(最长18) + 复位 \x1b[0m(4) + \r\n(2) = 最长 29B
帧上限 127B → 内容建议 ≤98B（超出截尾）
```

### 3.2 数据结构

```cpp
struct TxFrame        // 发送帧（三档共用，已格式化）
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

struct LogRecord      // 日志原始请求（未格式化，异步段生产队列条目）
{
    const char* fmt;               // 格式串（静态字面量，禁止临时栈串）
    LogColor    color;             // 颜色（四色）
    TxPriority  prio;              // 展开后的发送优先级
    bool        is_stale;          // DBG 切换/关闭时标记作废
    uint32_t    args[kMaxLogArgs]; // 参数快照（%f 占 2 槽，%s/%p 指针 1 槽，整型 1 槽）
    uint8_t     nargs;             // 参数槽数（0~kMaxLogArgs）
    uint8_t     next;              // 记录链表（数组索引，255=nullptr）
};
```

### 3.3 链表机制：三类链，零动态分配

整个日志系统的队列全部是**静态数组 + 数组索引链表**（`uint8_t next` 存数组下标，`255`=空），运行期不 new/malloc。裸链表字段已经封装到两个私有队列结构里：

**① `TxFrameQueue txq_`**：内部维护 `frames/free_head/head/tail`，负责发送帧池、三档优先级插队、池满挤最低优先级帧、跳过作废 DBG 帧。

**② `LogRecordQueue recq_`**：内部维护 `records/free_head/head/tail`，负责异步段未格式化请求的 FIFO、池满丢弃、DBG 作废标记，以及格式化完成后的回收。

帧在 `txq_` 内流转（分配→入队→发送→回收），LogRecord 在 `recq_` 内流转（快照入队→shell 出队→格式化入帧池→回空闲）。静态池 + 索引链表 = 时间确定、无碎片；封装后 `Log` 只表达业务动作。

### 3.4 打印入口（分时段）

| 函数 | 路径 |
|------|------|
| `Inf/Err/Ok/Wrn`（[log.cpp](log.cpp)） | `PrintColor(color, fmt, ap)` |
| `Dbgl`（[log.cpp](log.cpp)） | `e != active_` 静默；否则分时段走快照或直发（DBG 白色） |
| `SendLine`（[log.cpp](log.cpp)） | 命令响应直发（不经过 log 过滤，带 `\r\n`，Cmd 档）——非实时路径，保持 vsnprintf |

`PrintColor` 内部按 **`shell_own_` 分时段**（详见 §3.5）：

```cpp
void Log::PrintColor(LogColor c, const char* fmt, va_list ap)
{
    if (!shell_own_)                          // 同步段（boot 早期）：调用点直发，现状不变
    {
        char buf[kLogBufSize];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        char out[kLogBufSize + kColorOutExtra];
        int n = snprintf(out, sizeof(out), "\x1b[38;2;R;G;Bm%s\x1b[0m\r\n", ..., buf);
        if (n > 0) PublishDirect(out, n, TxPriority::Event);
        return;
    }

    // 异步段（shell 接管后）：参数快照入队，调用点不格式化
    uint32_t args[kMaxLogArgs];
    uint8_t  nargs = 0;
    SnapshotArgs(fmt, ap, args, &nargs);
    TryEnqueueRecord(fmt, c, TxPriority::Event, args, nargs);
    unsigned key = irq_lock();
    if (!sending_ && stream_ != nullptr) k_sem_give(&stream_->sem_);
    irq_unlock(key);
}
```

### 3.5 分时段同步异步发送（核心机制）

**背景**：shell 线程在 `PreThread` 阶段才启动（[shell.cpp](shell.cpp) `REGISTER_THREAD(thread_start, PreThread, ...)`）。boot 早期 shell 线程还没跑，若调用点只入队会无人消费（帧积压满后吞日志）。所以用 `shell_own_` 标志划分两个时段：

| 时段 | `shell_own_` | 调用点行为 | 实时要求 |
|------|-------------|-----------|---------|
| **同步段**（boot 早期） | `false` | `vsnprintf` + 入帧池 + **调用点直发** `SendFrame` | 无（启动阶段） |
| **异步段**（shell 接管后） | `true` | **参数快照**入 LogRecord 队列 + give `sem_`，不格式化 | 有（ISR/实时任务可能在此调用） |

- `shell_own_` 在 `Shell::Task()` 首行 `Log::SetShellOwn()` 置 `true`（[shell.cpp](shell.cpp)），只执行一次。
- **同步段日志即时出、不积压**（调用点直接 DMA 提交），避免 boot 日志吞帧。
- **异步段 ISR 只做快照+入队+give**（~µs 级），格式化与 DMA 全在 shell 线程。

### 3.6 发送仲裁（核心）

#### TxFrameQueue：帧池 + 三档优先级队列

`Log::EnqueueFrame()` 是锁内薄封装，真正的帧池分配、池满挤出、优先级插队都在 `TxFrameQueue` 内部完成：

```cpp
bool Log::EnqueueFrame(const char* data, int len, TxPriority prio)
{
    return txq_.PushLocked(data, len, prio, !k_is_in_isr());
}
```

`PushLocked()` 的步骤：截断 → 从 `free_head` 取帧 → 池满处理（ISR 直接丢 / 线程可挤最低优先级）→ 拷内容 → `PushByPrioLocked()` 按档插队。

规则：
- **Event(0) 插队头**——最高，永远最先发。
- **Cmd(1) 插事件后、DBG 前**。
- **Dbg(2) 排队尾**——最低。
- **同档先来后到**：`while (frames[*pp].prio <= f->prio)` 跳过同档，插到第一个更低优先级帧之前。

#### EvictLowestLocked：池满挤帧

```cpp
TxFrame* Log::TxFrameQueue::EvictLowestLocked()
{
    // 遍历队列找 prio 值最大（最低优先）的非 Event 帧，从链表摘除返回
    // 优先挤 DBG，其次 Cmd；全是事件帧 → 返回 nullptr（无可挤）
}
```

- **事件永不挤**（Event 帧在队列里不会被 Evict 掉）。
- 优先挤 DBG，其次 Cmd。被挤掉的帧直接复用为新帧。

#### 发送链（SendFrame / OnTxDone / PumpSend）

```
SendFrame(f)
  sending_ = true
  stream_->Send(f->data, f->len)      ← Stream 子类拷贝到自身 TX 缓冲后提交 DMA/USB
  txq_.ReleaseLocked(f)               ← 无论成败立即归还发送帧池

OnTxDone()——TX_DONE 中断回调（ISR 上下文）
  irq_lock()
  sending_ = false
  irq_unlock()
  if (stream_ != nullptr) k_sem_give(&stream_->sem_)   ← 通知 shell 线程续发

PumpSend()——shell 线程驱动（DMA 空闲则续发下一帧）
  unsigned key = irq_lock();
  if (!sending_) { TxFrame* f = Dequeue(); if (f != nullptr) SendFrame(f); }
  irq_unlock(key);
```

**关键点：帧即取即还**——当前 Stream 子类（如 `UartDma::Send`）内部把帧内容拷进自己的发送缓冲，DMA 搬运的是 Stream 子类缓冲而非 `TxFrame::data`。Send 提交成功即帧使命结束，立即回空闲池，**帧池稳态不被 DMA 占住**。

**发送驱动在 shell 线程**：异步段调用点只入队 + give；`OnTxDone` ISR 只清标志 + give；shell `Task()` 唤醒后 `PumpSend()` 提交下一帧。ISR 里不续发链。

#### Dequeue：队列弹头

```cpp
TxFrame* Log::Dequeue()
{
    return txq_.PopLocked();
}
```

`PopLocked()` 会跳过并回收 `is_stale` DBG 帧（`log on B` 时残留的 A 帧 / `log off` 残留），不发送——**DBG 切换及时顶替**。

### 3.7 参数快照（异步段，阶段3）

**目标**：异步段调用点（可能 ISR）连 vsnprintf 都不做，只把 `fmt + 参数值` 快照入 LogRecord 队列，格式化在 shell 线程。

**为什么快照而非存 va_list**：`va_list` 是栈指针，调用点函数一返回栈即回收，跨线程延迟使用是野指针。快照存的是**值拷贝**，不依赖调用点栈存活。

#### SnapshotArgs：参数值快照（调用点，va_end 前）

```cpp
void Log::SnapshotArgs(const char* fmt, va_list ap, uint32_t* args, uint8_t* nargs)
{
    uint8_t n = 0;
    while (*fmt && n <= kMaxLogArgs)   // n==kMaxLogArgs 时仍扫（消费参数防失步），不写槽
    {
        if (*fmt != '%') { fmt++; continue; }
        fmt++;                                    // 跳过 '%'
        if (*fmt == '%') { fmt++; continue; }     // "%%" 转义：无参数
        // 跳过 flags/width/.prec/length
        // 按转换符 va_arg：
        //   %d/%i/%u/%x/%X/%c → va_arg(int)  存 1 槽
        //   %f/%F → va_arg(double) 存 2 槽（位模式，little-endian 低/高 32 位）
        //   %s/%p → va_arg(void*) 存 1 槽（指针）
        ...
    }
    *nargs = (n <= kMaxLogArgs) ? n : kMaxLogArgs;
}
```

- **类型严格匹配 va_arg**，防失步；超 `kMaxLogArgs` 停止写槽但**继续消费参数**（防调用点 va_list 状态与 FormatRecord 失步）。
- **`%f` 占 2 槽**：double 64 位拆成低/高 32 位存两槽，展开时拼回。
- 支持转换符：`%d %i %u %x %X %c %f %F %lf %s %p`。`%%` 无参数。禁用 `%lld/%e/%g/%n`。

#### TryEnqueueRecord：入队（LogRecordQueue FIFO 尾插）

```cpp
void Log::TryEnqueueRecord(const char* fmt, LogColor color, TxPriority prio,
                           const uint32_t* args, uint8_t nargs)
{
    unsigned key = irq_lock();
    (void)recq_.PushLocked(fmt, color, prio, args, nargs);
    irq_unlock(key);
}
```

`LogRecordQueue::PushLocked()` 池满直接丢（O(1)），记录 `fmt/color/prio/nargs/args`，并用 `tail` 尾插保持 FIFO。DBG 记录保存为 `TxPriority::Dbg`，普通四色日志保存为 `TxPriority::Event`。

#### ProcessOneRecord / FormatRecord：消费端展开（shell 线程）

```cpp
bool Log::ProcessOneRecord()
{
    unsigned key = irq_lock();
    LogRecord* r = recq_.PopLocked();
    irq_unlock(key);

    if (r == nullptr) return false;

    FormatRecord(r);               // 锁外格式化

    key = irq_lock();
    recq_.ReleaseLocked(r);         // 格式化后才归还，避免被 ISR 提前复用
    irq_unlock(key);
    return true;
}

void Log::FormatRecord(LogRecord* r)
{
    // 扫 fmt：普通字符直接拷；% 段收集到 seg，按转换符从 args 取槽
    //   %d/%u/... → snprintf(seg, int)
    //   %f        → memcpy 2 槽拼回 double → snprintf(seg, double)
    //   %s        → 指针还原，null 转 "(null)"
    //   %p        → 指针还原
    // ANSI 上色 + \r\n → PublishQueued(..., r->prio)
}
```

- 展开在 shell 线程，用 picolibc snprintf 逐段展开（%f 精度由 libc 保证），不依赖 libc va_list。
- `FormatRecord()` 是 `Log` 私有实现细节；shell 线程不再拿裸 `LogRecord*`。
- **`%s` 约束**：快照存指针，延迟格式化要求指针仍有效——**只许字符串字面量**（静态存储期）。临时栈 `char buf[]` 传入会读野指针（最高风险，文档约束 + review 把关）。

### 3.8 并发保护

两个队列的共享状态被**任务上下文**（`PublishDirect/PublishQueued/SendLine/PrintColor/Dbgl/TryEnqueueRecord`）与 **ISR 上下文**（`OnTxDone/ISR 中 TryEnqueueRecord`）同时访问：

| 入口 | 锁 | 说明 |
|------|----|------|
| `PublishDirect/PublishQueued` | `irq_lock` | `EnqueueFrame` 和发送触发在锁内 |
| `OnTxDone` | `irq_lock` | 清 sending_，give 在锁外 |
| `PumpSend` | `irq_lock` | Dequeue+SendFrame 锁内 |
| `TryEnqueueRecord` | `irq_lock` | LogRecord 入队锁内 |
| `ProcessOneRecord` | `irq_lock`（出队/归还） | 格式化在锁外；记录格式化后才归还 |
| `MarkStaleDbg` | `irq_lock` | DBG 帧和 DBG 原始记录一起标记作废 |

加锁点集中在**入口**，内部 helper（`TxFrameQueue::*Locked` / `LogRecordQueue::*Locked` / `SendFrame`）只在锁内被调用，`FormatRecord()` 不持锁。

### 3.9 初始化生命周期

```
shell thread_init（[shell.cpp](shell.cpp)，PreInit 阶段）
  UartDma::Config cfg; cfg.base_cfg.tx_cb = Log::OnTxDone;
  rx.Init(DEVICE_DT_GET(DT_ALIAS(shell_uart)), cfg)   ← shell-uart = uart3
  shell_.Init(rx)                        ← Stream& 绑定
  DUST_LOG_INF("shell init\n")
  Log::Init()          ← 清 active_/count_/sending_，重建空闲帧链（0→1→2→3→255）+ rec 空闲链
  Log::BindStream(&rx) ← 绑定发送通道
```

`Log::Init()` 之后任何上下文（含 main 的 System_Startup、ISR）都能安全调用 DUST_LOG_*。

**boot 早期调用点直发**（`shell_own_=false`），`PreThread` 时 shell 线程启动，`Task()` 首行置 `shell_own_=true` → 切异步段。

### 3.10 log 命令

```
log list            遍历 entries_，输出 "名字 [ON]/[off]"
log on <name>       Select → "log on: ok" / "log on: not found"
log off             Deselect → "log off: ok"
```

---

## 4. var 模块详解

### 4.1 类型系统（[var.hpp](var.hpp)）

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

### 4.3 命令实现（[var.cpp](var.cpp)）

| 函数 | 行为 |
|------|------|
| `Find(name)` | 遍历 `.shell_var` 段按名字匹配 |
| `CmdList()` | 遍历段输出 `名字 (类型) = 值` + `--- N variables ---` |
| `CmdGet(name)` | 输出 `名字 = 值`；找不到 `not found: <name>` |
| `CmdSet(name, val)` | 按类型解析值字符串并写入 |
| `PrintVar` / `PrintValueOnly` | 11 种类型分支打印（float/double 转 double 后 `%f`） |

**数值解析**（`CmdSet`）：无符号 `strtoull`（支持 0x），有符号 `strtoll`，浮点 `strtof/strtod`，布尔 `true/1|false/0`；`*end != '\0'` → `bad value`。整数类型**不做范围检查**（`var set t_u8 300` 写入 300&0xFF=44）。

所有输出走 `Log::SendLine`（Cmd 档）。

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
    Log::SetShellOwn();                        // 首次运行接管发送（置 shell_own_=true）
    DUST_LOG_OK("shell send owner taken\n");   // 发送模式切换提示

    for (;;)
    {
        k_sem_take(&stream_->sem_, K_FOREVER);   // 通道事件：接收数据 OR 日志发送需要驱动

        Log::PumpSend();                          // 先驱动日志发送（DMA 空闲则续发）

        while (Log::ProcessOneRecord())           // 出队参数快照请求 → 展开入帧池 → 泵
        {
            Log::PumpSend();
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
1. `sem_` 从"接收通知信号量"升级为**"通道事件信号量"**（接收数据 OR 日志发送需要驱动，[stream.hpp](E:/Zephyr/zephyr_user/framework/drivers/communication/stream/stream.hpp) 注释同步）。
2. 唤醒后**先 `PumpSend()`** 泵日志帧，再通过 **`ProcessOneRecord()`** 展开异步段参数快照，最后才 `Read` 接收数据。
3. `sem_` 是 limit=1 计数信号量，接收/发送一起 give 会合并成一次唤醒——但安全：一次唤醒顺序执行全部处理，**数据不依赖 sem_ 计数**（接收数据在 Stream 缓冲、日志帧在队列，sem_ 只是"醒来处理"通知）。
4. **前提**：shell 线程是 `stream_` 的**唯一消费者**（remote/gimbal 各自持有自己的 Stream 实例，不共享）。

### 5.3 命令分发

```
解析第一个 token：
  "var" → Var::Process(line)     （var list/get/set）
  "log" → Log::Process(line)     （log list/on/off）
  "h"/"?" → CmdHelp()
  其他 → Log::SendLine("?: var/log/h")
```

### 5.4 初始化与注册

```
REGISTER_INIT  (thread_init,  PreInit, High, HaltOnFail, "dbg_init")    ← PreInit 初始化 uart3 + Log
REGISTER_THREAD(thread_start, PreThread, "dbg_start")                    ← PreThread 启动 shell 线程
```

`thread_init` 用 `DT_ALIAS(shell_uart)`（overlay 配 `shell-uart = &uart3`），注册 `tx_cb = Log::OnTxDone` → `Log::Init()` → `Log::BindStream(&rx)`。

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
| `log off` | `log off: ok` |

---

## 7. 容量模型与实测数据（2026-08-06，921600 波特）

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

## 8. FAQ

**Q：DUST_LOG 打印依赖线程吗？**
A：**分时段**。boot 早期（`shell_own_=false`）调用点直发，不依赖线程；shell 线程启动（`PreThread`）接管后走异步，依赖 shell 线程 `PumpSend` 驱动 DMA + `ProcessOneRecord()` 展开快照。唯一前置是 `Log::Init()/BindStream()` 已执行（`dbg_init`，PreInit 阶段）。

**Q：异步段调用点（ISR）会做什么？**
A：`SnapshotArgs`（扫 fmt + va_arg，~µs 级）+ `TryEnqueueRecord`（短临界区入队）+ give。**无 vsnprintf、无 DMA 提交、无链表遍历挤帧**。

**Q：为什么线程里不用 vsnprintf，要 FormatRecord 逐段拼？**
A：vsnprintf 需要 `va_list`，而 `va_list` 是栈指针、调用点函数返回即失效，传不到线程。快照存的是**参数值**，FormatRecord 只能按槽位约定用 snprintf 逐段展开。

**Q：帧池为什么是 8 帧？会不会不够？**
A：8×128B=1KB。帧即取即还（Send 提交即回收），稳态限速下不被 DMA 长时间占住；瞬时突发上限 = 帧数+1 = 9 条，第 10 条起丢（事件永不挤，DBG 先被挤）。

**Q：参数快照的 `%s` 有坑吗？**
A：有。快照存指针，延迟格式化要求指针仍有效——**只许字符串字面量**。临时栈 `char buf[]` 传入会读野指针（最高风险）。项目盘点 + review 把关。

**Q：boot 早期日志为什么是直发？**
A：shell 线程 `PreThread` 才启动，boot 早期若只入队会无人消费，帧积压满后 Early/Mid/Late 阶段日志全被吞（阶段2 回归诊断的教训）。同步段调用点直发保证即时出、不积压。

**Q：为什么 `log on` 不存在的名字回 not found？**
A：`Select` 只选中**已存在**条目（`DUST_LOG_DBG("name",...)` 注册过的）。用 FindOrCreate 会凭空创建幽灵条目——已修复。

**Q：`var set t_u8 300` 为什么是 ok？**
A：整数类型不做范围检查（写入 300&0xFF=44），只保证解析正确不崩溃。范围校验不属于 var 命令职责。

**Q：命令响应会不会被日志挤掉？**
A：命令响应走 Cmd 档（事件后、DBG 前），DBG 流式打印期间命令响应**先于**后续 DBG 帧显示；事件（INF/ERR/OK/WRN）永不丢。

**Q：`sem_` 是 limit=1，接收和发送一起 give 会不会丢事件？**
A：不会。合并成一次唤醒但顺序执行 `PumpSend`+`ProcessOneRecord`+`Read`，发送与接收都被处理；数据在队列/缓冲里不依赖 sem_ 计数，最多延迟到下次处理。

**Q：CONFIG_DUST_CMD_SHELL_LOG 关闭时调用 DUST_LOG_* 会怎样？**
A：宏为空（log.hpp `#else` 空宏），调用点零开销、编译不报错——与 Zephyr LOG=n 静默语义一致。
