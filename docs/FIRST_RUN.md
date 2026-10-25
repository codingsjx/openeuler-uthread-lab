# 首次运行验证记录

验证日期：2026-10-25（Asia/Shanghai）。

## 环境

- VirtualBox 7.2.22，2 个虚拟 CPU，4096 MB 内存。
- openEuler 24.03 LTS-SP3，x86_64。
- GCC 12.3.1，GNU Make 4.4.1，Python 3.11.6。
- 系统从已有虚拟硬盘启动，未删除或重建原有分区。
- 项目通过只读数据光盘传入，复制到 `/home/oslab/oslab-project/uthread-lab` 编译。

## 执行与观察结果

```sh
cd /home/oslab/oslab-project/uthread-lab
make check
```

两条 C 编译命令成功，终端构建输出未显示编译警告。
运行时测试输出：

```text
PASS: EOF wakeup, deadline, invalid descriptor, blocking-fd rejection, reinitialization
```

实验脚本最终 summary.json 的 status 为 passed。这意味着下列真实程序输出满足脚本断言：

- first-ready 模式的标记顺序为 AAAABBBBCCCC。
- round-robin 模式的标记顺序为 ABCABCABCABC。
- 所有事件时间戳单调，未报告事件缓冲溢出。
- 阻塞读模式：读结束之前工作任务推进 0 次。
- 事件驱动读模式：读结束之前工作任务推进 40 次。
- 两种 I/O 模式最终均记录全部 40 次工作任务推进。

## 证据与保存位置

客户机中保留 `results/schedule-first.csv`、`schedule-rr.csv`、`io-blocking.csv`、`io-event.csv` 及 `summary.json`。

主机验证截图：`first-test-pass.png`（同目录）。本记录通过观察实际编译及测试输出确认结果；不声称已经把客户机原始 CSV 全量回传到主机。

## 结论与限制

本次证明第一阶段原型在该 openEuler 环境可编译、可运行，并通过预设行为验收。它是一次冒烟验证，不是多轮基准测试，也没有证明普遍性能优势。I/O 完成时间还受虚拟机调度和负载影响；不从本次数据计算加速倍数。

尚需完成：并发等待者及信号路径验证、压力实验、参数化重复实验、pthread 基线、轨迹可视化和正式参赛文档。
