# openEuler 用户级线程调度与 I/O 阻塞行为实验系统

基于 openEuler 的课程实践原型：实现用户级上下文切换、轮转调度和事件驱动等待，配套 pthread 对照、重复测量与离线轨迹报告。

## 队友从这里开始

这是操作系统课程实践项目，围绕“一个用户级线程阻塞，为什么会影响其他任务，以及如何通过事件驱动等待改善”开展实现与实验。

建议阅读顺序：

1. [设计说明](docs/DESIGN.md)：运行时的状态、调度和等待机制。
2. [第二阶段实验结果](docs/STAGE2.md)：做了什么、实测结果和结论边界。
3. [核心实现](src/uthread.c)与[接口](include/uthread.h)：结合实验理解代码。
4. [原始构建与验证日志](results/stage2-validation.log)：检查实际运行证据。

仓库是公开的，队友可直接阅读或下载；提交修改可通过 fork 和 Pull Request 协作。

```sh
git clone https://github.com/codingsjx/openeuler-uthread-lab.git
cd openeuler-uthread-lab
```

GitHub 文件页面不会直接执行报告中的交互代码。可点击 Code → Download ZIP，解压后用浏览器打开 `results/report.html`。

## 当前验证状态

- 已编写 C 运行时、对照实验和自动验收脚本。
- **2026-10-25 已在 openEuler 24.03 LTS-SP3 x86_64 虚拟机中完成 `make check`，首次验收通过。** GCC 12.3.1、GNU Make 4.4.1、Python 3.11.6；本次终端构建输出未显示编译警告。
- 本次阻塞版本在读结束前工作任务推进 0 次，事件驱动版本推进 40 次。只说明该场景下等待隔离有效，不构成统计性能结论。
- 详细记录见 [首次运行验证](docs/FIRST_RUN.md)。其他架构尚未验证。
- **第二阶段已完成**：并发读者与信号中断测试通过；加入 pthread 对照，完成每种方式 20 轮有效测量和 1 轮预热，保存全部原始数据。详见 [第二阶段结果](docs/STAGE2.md)。
- **运行时优化已通过 Linux 验证**：按需日志缓冲、紧凑事件记录、无等待时跳过多余扫描、单任务 yield 快速路径、任务槽位复用与 errno 隔离。openEuler 容器中完整日志的 32 任务调度基准耗时中位数降低约 11.76%；I/O 总耗时基本不变。环境、边界及原始结果见 [优化说明](docs/OPTIMIZATION.md)。
- **第二轮优化已验证**：增加就绪位图与按 fd 去重的持久等待表。相对第一轮，openEuler 容器中 31 读者共享管道的固定合成基准中位数下降 44.06%，独立管道下降 7.94%；纯调度变化很小。这不是通用应用加速比例，见 [第二轮说明与原始证据](docs/WAIT_OPTIMIZATION.md)。

## 直接查看成果

双击 `results/report.html` 可离线查看统计对比、状态时间线和程序事件；支持切换场景、播放/暂停、拖动进度、缩放与下载当前 CSV。该文件内嵌真实数据，不需要安装依赖或启动服务。

仓库保留第二阶段已经验证的一批样例结果；新的实验输出默认由 .gitignore 忽略，需确认后再选择加入版本控制。分发的第二阶段项目包也包含本次实际结果。

## 在 openEuler 中构建

```sh
# 在已克隆的项目根目录中执行
sudo dnf install -y gcc make python3 glibc-devel
make
make check
```

完整复现第二阶段：

```sh
sh scripts/validate_stage2.sh
```

单独运行重复实验和生成报告：

```sh
make benchmark
make report
# 可调整固定工作量和延迟，每次保存在独立目录
python3 scripts/benchmark.py --repeats 20 --steps 40 --iterations 100000 --delay-ms 100
# 同环境重新编译历史基线与优化版，再比较调度和 I/O 场景
make perf-check
# 以第一轮优化为冻结基线，比较就绪位图与共享 fd 等待表
make wait-perf-check
```

所有测量子进程继承同一个虚拟 CPU 亲和性，不需要管理员权限。只在目标 Linux 环境执行 C 实验；Python 数据校验测试可在主机运行。

依赖：Linux、libc 的 ucontext、C11 编译器、make 和 Python 3 标准库。无需 root 运行实验，不修改内核。
`make check` 每个程序有 10 秒外部超时。失败会停止，不生成通过结论。

## 三个对照实验

1. `schedule first`：始终选第一个 READY 任务，预期标记 `AAAABBBBCCCC`。
2. `schedule rr`：循环选择下一个 READY 任务，预期标记 `ABCABCABCABC`。
3. `io blocking` 对比 `io event`：独立子进程延迟写管道。阻塞版本中读者占住承载线程；事件版本中读者等待时，计算任务仍能推进。

```sh
mkdir -p results
./build/demo schedule rr results/rr.csv
./build/demo io blocking results/blocking.csv
./build/demo io event results/event.csv
```

日志为真实 CSV 事件：`time_us,tid,event,detail`。计时使用单调时钟，日志先存内存，实验结束后写出。
自动测试还覆盖等待期间写端关闭后的 EOF 唤醒、超时、无效 fd、拒绝阻塞 fd 和重复初始化。
第二阶段额外覆盖两个读者竞争同一管道，以及 SIGALRM 多次打断等待时仍能按期超时。
`results/summary.json` 仅报告本次冒烟验证，不能用作性能提升比例。若系统严重过载导致演示输入提前到达，事件场景断言可能失败，应检查原始轨迹和环境。

## 设计范围

- 单内核线程，最多同时占用 32 个任务槽位，独立 128 KiB 栈，协作式调度；已完成槽位支持后续批次复用。
- 用户任务必须主动 yield 或调用 `ut_read`。不主动让出的任务可阻止其他任务推进。
- 显式非阻塞读接口，支持 pipe/FIFO 等可就绪描述符，支持超时和 EOF。
- 等待后端第一版使用 `poll`：避免 select 的 FD_SETSIZE 限制，后续可按需要加入 epoll。
- 每个调度边界均检查 I/O，只有没有 READY 任务时才阻塞等待；不会用忙轮询等待空闲 I/O。
- 不拦截任意系统调用，不提供普通磁盘文件异步化、多核并行、抢占或线程故障隔离。
- 每个任务退出后由调度器回收栈。无保护页，栈溢出可能导致进程崩溃，不能运行不受信任代码。
- 默认完整记录从 256 条事件按需增长，最大 131072；溢出计数写入 CSV，并将导出标记为失败。
- 可用 `ut_init_ex(UT_ROUND_ROBIN, UT_TRACE_OFF)` 关闭记录；该模式不能提供轨迹，导出返回 ENOTSUP。`ut_get_stats` 可读取调度、poll、快速路径和缓冲统计。
- ucontext 不属于当前 POSIX 可移植接口；本原型固定目标 Linux 环境。

## 文件组织

| 文件 | 内容 |
| --- | --- |
| include/uthread.h | 对外接口和约束 |
| src/uthread.c | 生命周期、调度、等待、超时、日志 |
| examples/demo.c | 两种调度与两种 I/O 模式 |
| tests/test_runtime.c | EOF、超时与异常路径测试 |
| scripts/run_experiments.py | 运行实际程序并验证轨迹 |
| examples/benchmark.c | 同一固定工作量的三种执行方式 |
| scripts/benchmark.py | 随机平衡顺序、预热、重复测量和统计 |
| scripts/make_report.py | 从实际结果生成离线 HTML 报告 |
| scripts/validate_stage2.sh | 第二阶段完整复现入口 |
| docs/STAGE2.md | 第二阶段结果、方法和限制 |
| docs/OPTIMIZATION.md | 运行时优化、严格回归和同环境对照结果 |
| scripts/measure_optimization.py | 历史基线与优化版的同环境测量 |
| docs/WAIT_OPTIMIZATION.md | 第二轮位图、等待表与增量对照 |
| scripts/measure_wait_optimization.py | 第一轮与第二轮的同环境测量 |
| docs/PROVENANCE.md | 课程来源、自主实现范围与资料 |
| docs/NEXT_STEPS.md | 环境准备和下一步任务 |

## 后续工作

已完成基础运行时、扩展验收、重复测量、pthread 基线、离线轨迹报告，以及两轮运行时开销优化。CI 自动在 openEuler 容器和 Ubuntu 上进行严格编译、回归与同环境对照。下一步增加真实服务工作负载、独立检查资源回收与教学效果，再整理正式技术报告和演示视频。
