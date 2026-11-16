# accept 连续错误退避与资源恢复验收

执行日期：2026-11-16（Asia/Shanghai）。从独立临时目录重新构建并运行全部 **16 组检查**，最终 `full_acceptance=true`。accept 恢复专项 **39/39 项通过**，原有 122 项 HTTP 字节用例全部通过。真实 30 秒收发空闲期限和 60 秒请求头总期限重新等待，没有沿用旧结果。

完整原始报告：[reports/epoll-accept-backoff-2026-11-16.json](reports/epoll-accept-backoff-2026-11-16.json)。复现入口：

```bash
python3 tests/run_epoll_checks.py --report tests/reports/epoll-latest.json
```

只运行恢复专项：

```bash
python3 tests/run_epoll_checks.py --only accept-recovery
```

单组运行的 `full_acceptance=false`；只有默认的全部 16 组通过才算完整验收。

## 本轮改动与检查方法

- 服务器按每次监听事件最多调用 accept 32 次计数，失败和 EINTR 也计入。连续可恢复错误跨事件累计，达到 32 次后暂停接收一秒；成功、EAGAIN/EWOULDBLOCK 或进入暂停后清零。
- EMFILE、ENFILE、ENOBUFS、ENOMEM 均立即进入资源不足暂停分支。暂停期间继续推进已有客户端，收到退出信号后不再恢复监听。
- 测试阶段没有修改用户完成的 `src/`、`include/` 实现。`src/tcp_server.cpp` 的 SHA-256 为 `8ecad039b2d317c0567e583f4498f7ab34e5057bc3b91e8e66bf9e19e812747e`。
- 测试 shim 增加显式启用的有序 accept 结果序列：正整数注入 errno，0 调用真实 accept 并检查成功；记录单调时间、epoll_wait 边界和全部 accept 调用。正常生产构建不链接该 shim。
- 成功清零检查在准备阶段使用 SIGSTOP/SIGCONT，将两个客户端同时排入等待队列，避免中途真实 EAGAIN 掩盖缺失的成功清零。EAGAIN 清零由单独的注入场景检查。

## 最终完整验收

| 分组 | 结果 | 用时 |
| --- | --- | ---: |
| `regression`，122 项 | 通过 | 3.062 秒 |
| `helpers`，14 项 | 通过 | 0.005 秒 |
| `lifecycle` | 通过 | 0.062 秒 |
| `concurrency` | 通过 | 3.276 秒 |
| `faults` | 通过 | 0.203 秒 |
| `draining` | 通过 | 1.185 秒 |
| `oversize` | 通过 | 0.371 秒 |
| `fatal-cleanup`，8 项 | 通过 | 1.096 秒 |
| `capacity` | 通过 | 0.510 秒 |
| `request-logging` | 通过 | 0.014 秒 |
| `graceful-shutdown` | 通过 | 16.141 秒 |
| `accept-recovery`，39 项 | 通过 | 38.348 秒 |
| `logger-time`，3 项 | 通过 | 0.007 秒 |
| `receive-timeout` | 通过 | 30.156 秒 |
| `send-timeout` | 通过 | 50.113 秒 |
| `request-deadline` | 通过 | 60.129 秒 |

构建和检查没有出现非预期 stderr 或 UBSan 诊断。报告确认源码、测试输入和二进制在运行期间未变化；本次测试创建的服务器均已结束。

## 39 项恢复检查的结果

- 正常对照 1 项、九种单次连接/网络错误 9 项均通过，不因单次网络错误暂停或退出。
- 资源错误 7 项：EMFILE/ENFILE/ENOBUFS/ENOMEM 的单次错误，以及 EMFILE/ENOBUFS/ENOMEM 连续三次错误。已有客户端在暂停期间取得完整 200，等待客户端在恢复后取得完整 200；没有提前恢复。
- 连续错误和清零 13 项：ECONNABORTED 与 EINTR 的 31、32、65 次边界，ENETDOWN 的 65 次错误，混合错误 65 次；成功、EAGAIN、资源暂停后的清零均通过。
- 31 次连续错误不暂停，32 次暂停一次，65 次暂停两次。连续错误暂停日志记录的次数均为 32。
- EINTR 场景实测每次事件循环最多调用 accept 32 次；成功清零场景确实覆盖两个各含 32 次调用的批次。所有有序场景都核对每次 epoll_wait 之间的调用预算；暂停期间没有继续调用 accept。
- 显式注入场景的暂停/恢复间隔约为 1.019～1.054 秒。每轮都有对应的取消登记和恢复登记证据，已有客户端在首次恢复之前完成响应，最终 fd 恢复基线。
- 暂停期间真实 SIGINT/SIGTERM 共 6 项，分别从 EMFILE、连续网络错误、连续 EINTR 的暂停状态退出。跨过原恢复期限后仍未重新登记监听，没有重复 DEL；已有请求完成后返回 0。
- 暂停 DEL 失败和恢复 ADD 失败 2 项通过，均返回 1，不误报恢复成功；严重错误清理组另验证服务器函数返回前已经释放其拥有的 fd。
- 真实低 fd 上限 1 项：只将服务器子进程的 RLIMIT_NOFILE 降为 64，实际接收 57 个客户端后触发 EMFILE，并等待再次触发。已有客户端仍取得 200，释放 fd 后等待客户端取得 200；父进程限制保持原样。

## 测试范围

一般检查启用 `_GLIBCXX_ASSERTIONS` 和 UBSan。真实 fd 完全耗尽场景使用单独的 Release + assertions 构建，原因及 GCC 11 对照记录保留在 [RECOVERY_BASELINE.md](RECOVERY_BASELINE.md)。

ENFILE、ENOBUFS、ENOMEM 和有序 EAGAIN 使用显式注入，未实际耗尽整机文件或内存。除指定注入点外，连接、HTTP 收发、原始超时和进程信号均使用真实机制；测试只使用临时回环端口并清理自己创建的进程。

本轮完成的是功能回归，没有重新测量性能。2026-11-15 的性能结果仍对应其原始输入快照，见 [RECOVERY_BASELINE.md](RECOVERY_BASELINE.md) 和 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。

本轮原始临时目录为 `/tmp/cppserver-epoll-checks-zo7nhf__`；目录可能被系统清理，长期复现依据为仓库内脚本、输入哈希和 JSON 报告。
