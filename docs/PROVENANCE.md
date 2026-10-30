# 来源与自主实现记录

## 课程来源

选题来源于学校提供的 `应用层实验-学生版.zip`：

- lab4/lab4_3/user_thread_test.c：ucontext、独立栈、主动 yield 和用户态调度教学主题。
- lab4/lab4_4/pipe_communication.c：管道通信教学主题。
- lab7/lab7_2/fifo.c 与 select.c：阻塞/非阻塞及 I/O 多路复用主题。
- `openEuler应用层实验简介.pdf`：观察、修改、解释及实验证据要求。

本目录的运行时与实验程序重新编写，未直接复制整份课程框架。基于课程中“优先选择第一个 READY”的选择方式构造显式对照策略。自主实现包含循环选择、等待状态、poll 唤醒、期限检查、事件缓冲和自动轨迹验证。它是教学原型，不主张发明协程或事件循环。

课程资料的授权范围尚未确认。暂不将整个课程压缩包或讲义加入公开代码发布包；正式发布前由队伍确认学校授权及比赛要求。

## 技术参考

- https://man7.org/linux/man-pages/man3/swapcontext.3.html
- https://man7.org/linux/man-pages/man2/poll.2.html
- https://man7.org/linux/man-pages/man2/read.2.html
- https://docs.libuv.org/en/v1.x/design.html
- https://github.com/sustrik/libmill
- https://man7.org/linux/man-pages/man3/pthread_create.3.html
- https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html

## AI 使用记录

2026-10-25：本阶段通过 Codex 对话进行课程资料分析、选题调研、C 原型与验收脚本编写。任务提示：从现有课程实验延伸用户级线程调度与 I/O 阻塞行为实验，开始第一版实现。具体模型版本请以用户界面可核查记录为准，不填写无法核实的版本。

所有代码需由参赛成员理解、复核并在目标环境验证；正式报告保留主要交互记录并补充后续修改及测试结果。

第二阶段同日补充：Codex 编写并在 openEuler 中验证并发读者、信号中断测试，以及固定工作量的 pthread/用户线程对照、重复实验脚本和 HTML 轨迹报告。主要任务提示：“继续任务”；依据上一阶段承诺推进重复实验、pthread 对照和轨迹展示。全部测量来自目标虚拟机，原始结果与构建日志已保存；报告模板的布局调整在主机完成。

2026-10-26：用户要求开始优化。Codex 改进日志记录、调度循环与槽位复用，补充 errno 隔离、回归、冻结基线和 CI 对照。此阶段数据来自 GitHub Actions 的 openEuler 容器及 Ubuntu runner，与上一阶段本机虚拟机数据分开保存。getcontext 的严格编译警告通过隔离上下文准备函数解决，没有关闭相关警告。性能解释按完整记录和无记录模式分别陈述。

同日第二轮提示：“可以继续优化吗”。Codex 增加就绪位图、持久去重等待表、截止时间缓存及对应回归，冻结第一轮版本进行增量比较。Ubuntu 的强化 poll 重定向导致首次新增测试未观测到系统调用，随后同时拦截 poll 与 __poll_chk，并保留 libc 检查。该问题及修正可在分支历史与 CI 日志核对。

2026-10-29：用户要求继续任务。宿主虚拟机启动问题处理后，由用户手动登录 oslab；Codex 在独立客户机目录部署提交 f32c5ce，执行严格编译、全部回归和两套同环境对照，取回 620 条正式测量。保存变慢场景与完整样本，不用 CI 数据替代本机结果。数据经主机回环地址与客户机 NAT 网关临时传输，完成后关闭传输服务；没有发布认证信息或完整宿主诊断日志。
