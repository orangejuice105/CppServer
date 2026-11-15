# accept 错误恢复与日志时间转换验收

执行日期：2026-11-15（Asia/Shanghai）。从独立目录重新构建并运行全部 **16 组检查**，最终报告 `full_acceptance=true`。真实 30 秒收发空闲期限和 60 秒请求头期限均重新等待，没有沿用旧结果。

完整结果：[reports/epoll-recovery-2026-11-15.json](reports/epoll-recovery-2026-11-15.json)。默认入口：

```bash
python3 tests/run_epoll_checks.py --report tests/reports/epoll-latest.json
```

## 本轮测试改动

- `accept-fatal` 从模拟 EMFILE 改为 EBADF，继续验证致命错误的最终清理。EMFILE/ENFILE 使用暂停和恢复检查，不再要求服务器立即退出。
- 新增 `accept-recovery`，共 18 项检查：正常对照、9 种连接/网络错误、EMFILE/ENFILE、连续三次资源不足、暂停期间真实 SIGINT/SIGTERM、取消与恢复登记失败，以及真实低 fd 上限下的恢复。
- 严重错误清理探针增加暂停 DEL 失败和恢复 ADD 失败两种路径。在服务器函数返回后、测试进程退出前，检查客户端、监听和 epoll fd 均已释放，借用的信号 fd 仍归调用者持有。
- 新增 `logger-time`，检查正常时间、转换失败及失败后恢复。调用实际日志库并等待其正常析构，核对日志文件中的时间、级别和正文；没有替换日志队列或写文件逻辑。
- 新增故障注入和时间记录只参与测试构建。正常服务器不会链接这些包装函数；性能测试不启用这些故障开关。
- 本轮测试整理没有修改 `src/`、`include/` 中用户完成的实现。

## 最终完整验收

| 分组 | 结果 | 用时 |
| --- | --- | ---: |
| `regression`，122 项 HTTP 字节用例 | 通过 | 2.988 秒 |
| `helpers`，14 项收发及 epoll 检查 | 通过 | 0.004 秒 |
| `lifecycle` | 通过 | 0.051 秒 |
| `concurrency` | 通过 | 2.243 秒 |
| `faults` | 通过 | 0.189 秒 |
| `draining` | 通过 | 1.165 秒 |
| `oversize` | 通过 | 0.335 秒 |
| `fatal-cleanup`，8 项清理检查 | 通过 | 1.074 秒 |
| `capacity` | 通过 | 0.556 秒 |
| `request-logging` | 通过 | 0.014 秒 |
| `graceful-shutdown` | 通过 | 16.110 秒 |
| `accept-recovery`，18 项 | 通过 | 12.898 秒 |
| `logger-time`，3 项 | 通过 | 0.010 秒 |
| `receive-timeout` | 通过 | 30.120 秒 |
| `send-timeout` | 通过 | 50.047 秒 |
| `request-deadline` | 通过 | 60.093 秒 |

最终构建未出现编译警告；各检查未出现非预期 stderr 或运行时检测诊断。报告确认输入和二进制哈希未变化，所有测试服务器均已结束。

## 恢复行为

- 九种连接/网络错误均记录对应警告；已有不完整请求和等待接入的请求随后取得完整 200 响应，服务器继续服务并正常退出。
- 模拟 EMFILE/ENFILE 及连续三次资源不足时，已有客户端在暂停期间仍可完成请求。每次取消监听登记后按期恢复，实测暂停间隔约为 1.054～1.085 秒；恢复后等待客户端取得完整响应，fd 恢复基线。
- 暂停期间收到真实 SIGINT/SIGTERM 后，测试继续等待 1.25 秒，跨过原重试期限。服务器保持退出状态，没有恢复监听或重复取消登记；已有请求完成后返回 0。
- 真实耗尽场景只将一个服务器子进程的 fd 软硬上限降为 64。服务器持有 57 个客户端、全部 64 个 fd 被占用后，至少两次触发真实 EMFILE。已有客户端仍取得 200；释放一个 fd 后，等待连接成功接入并取得 200，最后 fd 恢复基线。
- 真实耗尽场景没有提前发送 HTTP 请求预热时间格式化。fd 用满后的第一次完整 HTTP 响应也通过检查。测试运行器和用户终端的 fd 上限没有改变；ENFILE 使用显式注入，不实际耗尽整机资源。
- 日志时间转换失败时，`[time-unavailable]`、级别及正文仍落盘；恢复转换后，后续日志重新使用正常本地时间。转换调用计数符合预期，没有递归提交日志。

## 构建与检测工具的边界

常规检查、模拟错误和日志时间检查使用 `_GLIBCXX_ASSERTIONS` + UBSan。真正将 fd 全部用满的检查使用独立 Release + assertions 构建。

首次运行中，真实耗尽场景触发 GCC 11 UBSan 的标准库 vptr 诊断，其余 15 组通过。该次结果原样保存在 [初次报告](reports/epoll-recovery-2026-11-15-initial.json)，不能作为完整验收通过依据。

随后使用不依赖项目代码的最小程序对比同一个 `std::put_time` 操作：未插入检测的冷启动执行通过；UBSan 冷启动且 fd 全满时出现同类诊断；先预热或释放两个 fd 后，UBSan 执行通过。完整源码、编译器版本及六次原始输出见 [诊断记录](reports/ubsan-fd-diagnostic-2026-11-15.json)。

[GCC 11.4 检测运行库源码](https://github.com/gcc-mirror/gcc/blob/releases/gcc-11.4.0/libsanitizer/sanitizer_common/sanitizer_posix_libcdep.cpp#L255-L275) 表明，`IsAccessibleMemoryRange()` 会申请 pipe，失败时返回 false。这与上述对照结果一致。因此真实耗尽场景改用生产 Release 构建，保留冷启动检查；其余场景继续启用 UBSan，没有全局关闭检测、忽略诊断或改变服务器实现。

本轮最终功能验收原始目录：`/tmp/cppserver-epoll-checks-z7p21pjf`。临时目录不保证长期保留，复现使用仓库内脚本和当前源码。

这些结果覆盖当前列明的场景；尚未进行 ThreadSanitizer、长期稳定性或公网容量验证。旧阶段记录见 [HARDENING_BASELINE.md](HARDENING_BASELINE.md)。

## 独立 Release 性能重测

原始结果：[reports/epoll-performance-2026-11-15.json](reports/epoll-performance-2026-11-15.json)。独立重新构建 Release，保留生产 INFO 异步文件日志、backlog、超时及关闭策略。每个请求重新建立连接；预热 0.5 秒，测量 3 秒，每种场景重复两次。与最终功能报告的源码和测试输入哈希完全相同。

| 路由 / 等待连接 | 发压并发 | 两次成功 QPS | 两次 p99 延迟范围 |
| --- | ---: | --- | --- |
| `/health` / 0 | 1 | 2380.14、2396.95 | 0.788～0.798 ms |
| `/health` / 0 | 16 | 4552.75、4407.77 | 3.974～4.083 ms |
| `/health` / 0 | 64 | 4122.29、4028.34 | 10.545～12.157 ms |
| `/hello` / 0 | 1 | 2418.12、2418.73 | 0.787～0.792 ms |
| `/hello` / 0 | 16 | 3662.22、4414.86 | 3.501～3.775 ms |
| `/hello` / 0 | 64 | 4128.30、4061.56 | 10.995～11.504 ms |
| `/health` / 256 | 64 | 4105.28、3960.68 | 11.755～11.983 ms |

14 次正常测量共验证 **179282 个完整响应**，请求错误为 0。全部 16 次测量的日志丢弃数均为 0，日志队列历史峰值最大为 48 条；各场景释放连接后 fd 恢复基线 7 个，健康请求正常，服务器正常退出。

1024 个名额已满的两次测量中，成功 HTTP QPS 均为 0；分别记录 20930、20219 次预期连接关闭。这些关闭不计入成功吞吐量。包括预热和探针在内的容量拒绝总数分别为 24382、23835，采样 fd 峰值均为 1032；释放名额后服务恢复。

正常负载下服务器 CPU 为单核的 48.66%～65.64%，客户端为 72.64%～96.11%，RSS 采样峰值为 3.5～3.75 MiB。完整 p50/p95/p99、CPU、内存、fd、日志和环境数据见 JSON。采样周期为 50 毫秒，可能漏掉更短峰值。

这些是同机回环网络、Python 发压端、短响应和短连接下的短时测量；没有据此确定服务器极限容量，也未完成长期生产负载验证。延迟包括建连、完整响应、服务器 FIN 和本地连接清理，吞吐量计入最后在途请求完成的时间。

```bash
python3 tests/run_epoll_benchmark.py --report tests/reports/epoll-performance-latest.json
```

本轮性能原始目录：`/tmp/cppserver-epoll-benchmark-pet86v10`。报告确认输入及二进制未变化，测试自己的服务器全部停止。2026-11-14 的历史测量保留在 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。
