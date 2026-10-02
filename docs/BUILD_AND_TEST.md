# 构建与测试

[返回 README](../README.md) · [架构与接口边界](ARCHITECTURE.md) · [验证记录](VERIFICATION.md)

本文对应 [core/Makefile](../core/Makefile) 的现有目标。不包含 Qt、内核、U-Boot、固件镜像或部署构建步骤，因为仓库未提供这些工程。

## 1. 环境要求

- 推荐 Linux 宿主或 Windows 的 WSL Linux shell；完整核心含 `termios`、`poll`、`sys/ioctl.h`、`unistd.h` 与 Unix `socketpair()`。
- GNU Make、GCC 及 Linux/POSIX C 开发头文件。
- 宿主测试链接数学库 `-lm`，不使用第三方测试框架。
- 不需要 Qt、SQLite、Python、libgpiod 或真实串口硬件来运行现有宿主测试。

Ubuntu 环境尚未安装工具链时，可执行：

```bash
sudo apt-get update
sudo apt-get install --yes build-essential
```

这是环境准备命令，不是本轮已执行记录。不声明固定编译器版本；采集证据时应记录实际 `gcc --version` 与 `make --version`。

## 2. 从源码构建并运行

下面命令均从仓库根目录执行，使用 Linux shell：

```bash
make -C core clean
make -C core test CC=gcc
```

先 `clean` 可避免复用已存在的 `core/build/test_core` 或陈旧对象。本 Makefile 没有自动头文件依赖跟踪，也不跟踪编译器/编译参数变化；修改头文件或切换参数后同样应清理。

等价的默认目标：

```bash
make -C core CC=gcc
```

`all: test`，所以默认目标也会运行测试。显式传 `CC=gcc` 可避免 GNU Make 内建的 `CC=cc` 影响 `CC ?= gcc` 的实际选择。

| 目标 | 实际用途 | 产物 / 副作用 |
|---|---|---|
| `all` | 默认转到 `test` | 构建并运行测试，不是只编译 |
| `test` | 需要时构建对象与测试程序，然后执行 | `core/build/*.o`、`core/build/test_core` |
| `cross` | 使用 `CROSS_CC` 编译核心，再归档 | `core/build/cross/*.o`、`core/build/libgateway_core.a` |
| `clean` | 删除 `core/build/` | 不处理交叉编译失败时遗留在 `core/` 的 `.o` |

默认编译参数是：

```text
-std=c99 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
```

`test_core` 链接六个核心实现与 `test/test_core.c`、`test/test_rs485_port.c`。默认未使用 `-Werror`；“退出成功”不等于“没有警告”，需分别检查编译日志。

只构建测试可执行程序，不运行：

```bash
make -C core build/test_core CC=gcc
```

已有宿主产物时可单独执行：

```bash
./core/build/test_core
```

`rs485_port.c` 在系统头文件前定义 `_DEFAULT_SOURCE`，用于暴露 glibc 的 `cfmakeraw()`、`usleep()` 等接口；这不代表已验证所有 libc 或非 Linux 平台。

## 3. 测试入口与覆盖边界

聚合入口是 [test_core.c](../core/test/test_core.c) 的 `main()`；端口套件通过 `run_rs485_tests()`、`rs485_checks()` 和 `rs485_failed()` 合并计数。

| 套件 / 入口 | 源码中的检查 | 未覆盖或不应外推的结论 |
|---|---|---|
| `test_crc16()` | 按位与查表一致、CRC 字节序往返、错帧、增量、空输入 | 未直接断言注释中的固定 CRC 值；无并发首次初始化测试 |
| `test_modbus_rtu()` | `0x03` / `0x06` / `0x10` 请求字段、非法请求、读响应、CRC 错误、异常帧、错从站、短帧、写多响应 | 无完整事务、流式帧组装或系统性小缓冲区 / 极值 / 模糊测试 |
| `test_offline_cache()` | FIFO、序号、满时丢最旧与计数、头部 CRC 损坏、超长载荷 | 不验证磁盘持久化、掉电恢复、ACK 匹配或真实网络补传 |
| `test_reconnect_fsm()` | 重试动作、退避翻倍与封顶、补传优先、在线新数据动作、断链复位 | 测试手动设 `UPLINK_DRAINING`，不是成功回调自动迁移；无真实 socket |
| `test_register_map()` | 六种类型相关解码、缩放、数量不足、周期选点、隔离跳过、宽泛值检查 | CDAB 测试只断言与 ABCD 不同，未断言正确交换输入的已知数值；无配置文件/热加载 |
| `run_rs485_tests()` | socketpair 收发、DE 钩子顺序、读超时、队列复位释放 DE、非法参数 | 非 TTY 分支；不验证 TTY 配置、UART 排空、保护时间、GPIO、电气波形 |

“检查数”是执行时的断言计数，不是测试函数数量或覆盖率百分比。缓存与调度测试包含循环，不能通过简单统计 `CHECK` 文本行来推算一次运行的数量。

### 解读结果

程序输出各套件名称以及末尾的 `checks`、`failed`、`result`：

- 断言失败会打印 `[FAIL]`、文件名、行号与条件。
- `failed` 为 0 时程序返回 0，否则返回 1；Make 会据此判断测试步骤成功或失败。
- `socketpair()` 创建失败时端口套件打印 `[SKIP] socketpair unavailable` 并返回，不自动增加失败计数。即使看到 `ALL PASS`，也应检查是否存在跳过。
- 没有覆盖率、硬件性能、连续运行时长或真实现场恢复指标的统计。

2026-10-02 在 WSL Ubuntu-22.04 / GCC 11.4.0 执行宿主验证，命令为 `make -C core -B test`，结果为 170 checks、0 failed。`-B` 强制重建目标，区别于上面的 clean / test 两步命令。证据来源和未验证范围见 [VERIFICATION.md](VERIFICATION.md)；通过结果不等于零警告或板级验收。

## 4. 保存可复核证据

在 Linux shell 的仓库根目录，以下命令可记录工具版本、干净构建和退出状态：

```bash
gcc --version
make --version
make -C core clean

set -o pipefail
make -C core test CC=gcc 2>&1 | tee core-test.log
```

这里的 `set -o pipefail` 用于 Bash，避免 `tee` 的成功掩盖 Make 的失败。`core-test.log` 属于忽略的日志；发布结果时记录环境、命令、实际汇总、警告和跳过情况，不发布密钥或本地配置。

可选的 GCC 地址/未定义行为检查：

```bash
make -C core clean
make -C core test CC=gcc \
  CFLAGS='-std=c99 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fsanitize=address,undefined -fno-omit-frame-pointer'
```

这使用现有 `CFLAGS` 覆盖机制，不新增 Makefile 目标；不是默认 CI，也没有在本轮运行。其范围仍限于现有测试实际执行的路径。

## 5. 现有交叉编译目标

在已安装并配置匹配 ARM Linux ABI 的工具链时：

```bash
make -C core cross CROSS_CC=arm-linux-gnueabihf-gcc
```

`CROSS_CC` 的默认值也是 `arm-linux-gnueabihf-gcc`。预期输出为 `core/build/libgateway_core.a`，不是可启动固件或完整网关应用。

必须注意 [Makefile](../core/Makefile) 的实际实现：

1. `cross` 先编译 `SRCS`，移动 `.o` 到 `build/cross/`，再用 `src/*.c` 编译一遍。
2. 归档使用硬编码的宿主 `ar`，没有 `CROSS_AR` 变量或工具链前缀传递；需要实际确认其支持目标对象格式，不能宣称跨平台归档已经验证。
3. 编译失败可能留下 `core/*.o`；这些文件已被 `.gitignore` 排除，但 `clean` 只删除 `build/`。
4. 未提供 sysroot、BSP、目标应用链接、板级 DE 实现、设备树、启动镜像或烧录步骤。T113 是原设想的接入方向，不是现有板级支持承诺。

本轮未执行交叉编译。不要用宿主单元测试成功替代目标 ABI、板卡或真实串口验证。

## 6. CI

[.github/workflows/ci.yml](../.github/workflows/ci.yml) 提供一个 Ubuntu 宿主任务：

- 触发：`push`、`pull_request`、`workflow_dispatch`。
- 权限：`contents: read`。
- 检出：`actions/checkout@v4`。
- 超时：5 分钟。
- 执行：`make -C core clean`，随后 `make -C core test CC=gcc`。

CI 只验证现有核心的宿主编译和测试退出状态，不生成固件、不发布产物、不上传代码，也不测量硬件。配置文件存在不代表远端已经执行；结果以对应工作流运行日志为准。
