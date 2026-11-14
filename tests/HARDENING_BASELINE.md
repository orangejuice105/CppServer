# 连接容量、请求日志与优雅退出验收

执行日期：2026-11-14（Asia/Shanghai）。最终脚本从独立目录重新构建并运行全部 **14 组检查**，报告 `full_acceptance=true`。实际 30/60 秒期限全部重新等待，没有沿用历史结果。

完整结果：[reports/epoll-hardening-2026-11-14.json](reports/epoll-hardening-2026-11-14.json)。独立 Release 性能结果见 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。两份报告的源码和测试输入哈希完全一致，运行前后输入与二进制未变化，测试自己的服务器全部停止。

## 本轮改动

- 测试入口按生产顺序准备信号集合、创建 signalfd，将其传入 `runTcpServer(shutdownFd)`，最后由调用者关闭。严重错误探针同时检查服务器没有关闭借用的信号 fd。
- 两套故障注入工具将信号 fd 与客户端区分，防止把启动阶段的信号登记误算成客户端登记或误注入客户端故障。
- 新增可重复运行的容量、日志隐私和真实信号退出检查。容量测试分批准备等待连接，避免 backlog 引起的建连重传拖长准备时间；生产配置和时钟保持原样。
- 增加 Release 性能脚本、仅测试使用的日志统计探针和对应 CMake 目标。
- 本轮没有修改 `src/` 或 `include/` 中的服务器实现。

## 最终完整验收

| 分组 | 结果 | 用时 |
| --- | --- | ---: |
| `regression`，原有 122 项 HTTP 字节用例 | 通过 | 2.95 秒 |
| `helpers`，14 项收发、清理及 epoll 状态检查 | 通过 | 0.00 秒 |
| `lifecycle` | 通过 | 0.05 秒 |
| `concurrency` | 通过 | 2.23 秒 |
| `faults` | 通过 | 0.20 秒 |
| `draining` | 通过 | 1.17 秒 |
| `oversize` | 通过 | 0.39 秒 |
| `fatal-cleanup` | 通过 | 0.03 秒 |
| `capacity` | 通过 | 0.55 秒 |
| `request-logging` | 通过 | 0.01 秒 |
| `graceful-shutdown`，13 个真实场景或明确标记的故障场景 | 通过 | 16.11 秒 |
| `receive-timeout` | 通过 | 30.10 秒 |
| `send-timeout` | 通过 | 50.05 秒 |
| `request-deadline` | 通过 | 60.04 秒 |

构建启用 `_GLIBCXX_ASSERTIONS` 和 UBSan；没有编译警告或运行时检测诊断。默认生产入口、测试辅助程序及 Release 性能入口都已实际构建。

## 关键观察

- 同时保留 1024 个等待连接后，额外 101 个连接全部关闭；容量告警只记录计数 1、100。释放一个名额后健康请求正常，最终 fd 从基线 7 个恢复到 7 个。
- 合法与格式错误的请求均携带人工生成的查询参数、Authorization、Cookie 和其他字段值标记。日志保留方法、路径、版本及 fd，没有输出这些标记；退出后日志已完成落盘。
- 空闲时真实 SIGINT/SIGTERM 均返回 0。两个已有请求在收到退出通知后仍可完成完整 200 响应；新连接没有进入应用接收和处理流程。重复信号没有重复删除监听登记。
- 不完整请求分别在第一次信号后 5.039、5.110 秒被结束；后者在第 2 秒收到第二个退出信号，总期限没有重置。
- 真实 TCP 发送背压场景使用测试专用 8 MiB 响应。仅交给内核 12288 字节后停住，退出信号后约 5.127 秒主动结束连接，日志记录剩余 1 个连接。此处预期响应未完成；期限内正常完成的响应另行验证。
- 信号 read 的 EINTR/EAGAIN 返回值注入后成功重试；读取失败、长度异常、信号 ADD 失败和监听 DEL 失败均记录错误并返回 1。它们属于显式故障注入，不声称真实 signalfd 会返回不完整记录。
- 实际接收空闲超时约 30.085 秒；GET/HEAD/HTTP/1.0 等响应符合预期。固定请求头期限约 60.010 秒，定期输入没有延长期限；第 50 秒补齐的请求正常响应。
- 两个真实发送停滞连接约在最后进展 30.039 秒后被扫描释放；等待期间健康请求耗时约 0.80 毫秒。辅助发送检查在第 20 秒恢复进展后约第 50 秒超时。

## 范围与复现

```bash
python3 tests/run_epoll_checks.py --report tests/reports/epoll-latest.json
```

停止接收阶段只删除监听 fd 的 epoll 登记，监听 socket 在退出时关闭，因此 TCP 握手仍可能成功；测试要求应用停止 accept 和处理新连接。约 100 毫秒的轮询扫描及调度开销会使期限处理稍晚于设定时间。

超限输入的 HTTP 响应完整性和 EOF/RST 分别记录；有界清理仍不保证任意输入均以 FIN 结束。UBSan、fd 检查和这些场景不证明所有可能路径无误；未进行 ThreadSanitizer、长期稳定性及未实现协议功能的检查。

关键输入 SHA-256（完整列表见 JSON）：

| 文件 | SHA-256 |
| --- | --- |
| `src/main.cpp` | `9f76abfbc2918ddf4327a364941c00beaccff1eb780495fb2aa49ade81c86643` |
| `src/tcp_server.cpp` | `756a638f31921b9391464df67b6687358acc1cf709fc753f08d8aa2ddf0061f7` |
| `src/tcp_connection.cpp` | `a2a021ef3b586ab50e3db49f1271298fd291fbee2e5f4bf3da48486486902fd6` |
| `CMakeLists.txt` | `082c8c941b86d506cde96310deae0d78f7570f74feac92173e8153cc922c9922` |

本机最终功能验收原始目录：`/tmp/cppserver-epoll-checks-qxd2gcqb`。临时目录不保证长期存在，测试脚本不依赖历史临时文件。
