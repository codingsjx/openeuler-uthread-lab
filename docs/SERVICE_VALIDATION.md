# Socket 场景与生命周期验证

在优化分支的 Linux 环境运行：

```sh
make service-check CFLAGS='-O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror'
```

结果保存在独立的 `results/service-时间-随机后缀/`。`service.json` 包含源码哈希、系统信息、子程序退出状态和六次场景样本，CSV 保存对应运行时轨迹。每种模式三次，交替执行顺序，不用三次样本进行通用性能排名。

## 请求/响应

四个独立客户端进程通过 AF_UNIX/SOCK_STREAM socketpair 与四个用户线程通信，每客户端八次请求，每次只允许一个未完成请求。客户端 0 在启动门打开后延迟 300 ms 再发首条请求；其他客户端立即开始。客户端校验每次回复后继续请求。

服务端先创建慢客户端任务。blocking 直接 read，event 使用 O_NONBLOCK 和 ut_read，两种模式都显式 yield。程序校验全部 32 条请求、回复与客户端退出状态。

`fast_responses_before_slow_request` 是服务端读取慢客户端首条请求前，其他客户端已完成回复的数量。blocking 通常为 0，event 通常大于 0；这是观测值，脚本不设置时延或加速比阈值。系统调度延迟可能改变观察，应保留原始数据。first_reply_us 是服务端发送首条回复的时间，不是客户端端到端时延。

这是本地、预先连接、有限载荷的机制实验，没有实现 TCP、listen/accept、任意长度协议或异步写。每个回复一字节且每连接仅一个未完成请求，不能据此推断一般写侧背压能力，也不能称为生产服务器。

## 生命周期

一个进程内执行 2000 次 init/spawn/run/destroy，每轮八个读者与一个写者。交替完整日志和关闭日志；写者分步写入并关闭管道，读者校验内容及 EOF，共 16000 条消息。每轮关闭应用端 fd 后通过 /proc/self/fd 检查数量恢复，并核对无日志模式不分配轨迹缓冲、两种模式均无丢失事件。

这验证重复生命周期、等待状态重置和应用 fd 回收，没有证明堆内存不存在泄漏，也不是长时间生产稳定性认证。更长时间压力和内存泄漏检查仍需另外验证。

程序设置超时，控制脚本超时后终止整个子进程组，避免残留客户端。编译/运行失败应检查日志，不能把脚本存在写成目标虚拟机已经验证。

## 2026-11-06 CI 记录

源码 `9864712` 的 [CI](https://github.com/codingsjx/openeuler-uthread-lab/actions/runs/42207684129) 在 openEuler 24.03 LTS-SP3 容器和 Ubuntu 均通过严格编译、既有回归、两套优化对照及新实验。每套环境三次 blocking 均观察到 0 条提前响应，三次 event 均为 24 条；每次全部 32 条回复及客户端校验通过。每套环境的生命周期验证均完成 2000 轮、16000 条消息，fd 数由 4 恢复到 4。

原始 JSON、CSV 和服务日志保存在 [CI 证据目录](../results/service-ci-20261106/)，与下述本机结果分别保存。

## 2026-11-06 本机 openEuler 虚拟机

本机恢复登录后，以 oslab（UID 1000）在独立目录部署 `fc71f4b`，通过 make check、四项 Python 报告测试和 make service-check，C 编译启用 -Werror。运行环境为 openEuler 24.03 LTS-SP3，内核 6.6.0-132.0.0.111，VirtualBox 2 vCPU、4 GiB。

六次场景全部通过 32 条回复及客户端退出校验；三次 blocking 提前回复数均为 0，三次 event 均为 24。生命周期完成 2000 轮、16000 条消息，fd 数由 4 恢复到 4。这支持当前受控场景的等待隔离和重复生命周期结论，不能证明一般网络服务性能或不存在堆泄漏。

原始数据、构建日志、环境、归档哈希及命令状态在 [本机证据目录](../results/service-vm-openeuler-20261106/)。源码部署在客户机 `/home/oslab/oslab-project/service-20261106T112207Z-1efad0/uthread-lab`，没有覆盖课程目录或之前的实验结果。
