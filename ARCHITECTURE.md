# cmd/ 架构说明

> **待完善** — 当前仅搭建了骨架（`CMakeLists.txt` + `Kconfig`），命令文件尚未实现。

## 职责

Zephyr shell 命令。代码运行时通过命令行修改参数值、查看状态。

## 边界

| 管 | 不管 |
|----|------|
| 注册 shell 命令，接收用户输入 | 不实现业务功能逻辑 |
| 解析参数并调用下层接口（`algorithm/`、`modules/`、`topic/`） | 不直接操作硬件 |
| 通过 Kconfig 控制哪些命令编译进固件 | 不定义线程 |

## 目录结构

```
cmd/
├── CMakeLists.txt
├── Kconfig
├── ARCHITECTURE.md
├── build/
└── shell/
```

build/
    独立编译脚本（bat / ps1），用于 Zephyr 环境外单独编译或离线检查。
    不属于固件的一部分。

shell/
    Zephyr shell 命令实现。每个文件通过 CONFIG_SHELL_XXX 控制编译。
    运行时通过串口终端修改参数、查看状态。

## 文件规范

命令文件放在 `shell/` 目录下，每个文件通过 `CONFIG_SHELL_XXX` 控制是否编译。

规则：
- 用 Zephyr `SHELL_CMD_REGISTER` 宏注册
- 只做参数解析 + 调用下层，不持有状态
- 新增命令：在 `shell/` 下建 `.cpp` → 在 `CMakeLists.txt` 加对应段 → 在 `Kconfig` 定义开关
- build/ 下的编译脚本用于 Zephyr 环境外独立使用，不属于固件的一部分

## 依赖关系

依赖 Zephyr shell 子系统（`CONFIG_SHELL`），在 `prj.conf` 中开启。

## 调用方

- 用户通过串口终端输入命令触发
- 内部调用 `algorithm/`、`modules/` 的接口完成参数修改或状态查询
