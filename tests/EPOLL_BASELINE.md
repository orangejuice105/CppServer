# LT epoll HTTP 最终验收记录

本文件保留 2026-11-09 基础 epoll 阶段的历史结果。新增连接容量、日志和优雅退出后的当前完整验收见 [HARDENING_BASELINE.md](HARDENING_BASELINE.md)，本机性能结果见 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。下文的 11 组通过记录仅对应当时的源码。

最终清理后复验：2026-11-09（Asia/Shanghai）。清理后的源码及新构建二进制完成全部 11 组检查，未沿用清理前或阻塞式版本的通过结果。

最新报告：[reports/epoll-final-2026-11-09.json](reports/epoll-final-2026-11-09.json)，`full_acceptance=true`。清理前报告 [epoll-2026-11-09.json](reports/epoll-2026-11-09.json) 仅记录此前源码快照。

## 本轮收尾

- 移除旧的 `handleClient()`、`sendAll()`、`sendRequestErrorResponse()`、`closeClientConnection()` 及旧入口声明，保留 epoll 和 HTTP 共享处理函数。
- 移除旧阻塞式 CMake 测试目标；线程池保留为按需构建的独立目标，服务器只链接日志库。
- 历史 Echo、请求头接收、阻塞式 HTTP 测试源码归档至 `tests/history/`。当前验收使用独立的 `http_test_support.py`，不再依赖旧脚本。
- 更新主 README、当前测试说明、相关注释和尾空格。期限、HTTP 功能及连接状态设计保持原有行为。

```bash
python3 tests/run_epoll_checks.py --report tests/reports/epoll-latest.json
```

## 最终复验结果

| 分组 | 覆盖 | 结果 | 用时 |
| --- | --- | --- | --- |
| `regression` | 122 个 HTTP 字节用例 | 通过 | 2.95 秒 |
| `helpers` | 14 项当前连接函数及 epoll 状态检查 | 通过 | 0.00 秒 |
| `lifecycle` | 40 次连接、半关闭、空 EOF、真实 RST、端口占用 | 通过 | 0.04 秒 |
| `concurrency` | 80 个等待连接；200 次请求、16 并发 | 通过 | 3.26 秒 |
| `faults` | 7 种显式系统调用故障注入 | 通过 | 0.19 秒 |
| `draining` | 空闲清理、固定 1 秒总期限、64 KiB 上限 | 通过 | 1.17 秒 |
| `oversize` | 20 项 GET/HEAD 目标边界及一次/分段对比 | 通过 | 0.40 秒 |
| `fatal-cleanup` | 6 项空容器、严重错误及失败清理检查 | 通过 | 0.03 秒 |
| `receive-timeout` | 5 种真实 30 秒接收空闲场景 | 通过 | 30.07 秒 |
| `send-timeout` | 真实发送停滞、恢复进展和两个 TCP 连接扫描释放 | 通过 | 50.04 秒 |
| `request-deadline` | 3 种真实 60 秒超时，以及第 50 秒完整请求 | 通过 | 60.05 秒 |

独立 CMake 构建启用 `_GLIBCXX_ASSERTIONS` 和 UBSan，无编译警告或运行时检测诊断。源码、测试代码及二进制哈希在本轮执行前后相同；测试自己的服务器全部停止。正常构建及线程池单独构建也已通过。

## 实际时间和资源观察

- 接收空闲期限：30.05～30.05 秒，空连接关闭及 GET/HEAD/HTTP/1.0 的 408 响应均符合预期。
- 请求头总期限：60.03 秒；第 50 秒补齐的请求在 50.01 秒正常响应。
- 真实满发送缓冲区：`PASS full-buffer-30s elapsed=30.018510 real_EAGAIN_returns=599`。
- 第 20 秒恢复发送后：`PASS progress-at-20s elapsed=50.030349 idle_after_last_progress=30.030217 accepted_bytes=16128`。
- 两个停止读取的 TCP 客户端，在最后一次发送进展后分别约 30.07、30.06 秒由扫描释放；等待期间健康请求正常响应。
- 无新事件的清理连接约 0.10 秒释放；持续小量输入下，清理仍在约 1.03 秒结束。
- 并发及各组恢复检查回到 fd 基线 6 个；严重错误探针在测试进程退出前检查所有客户端、监听和 epoll fd 已释放。

## 验收范围

- 普通网络组运行生产 main。发送专项的单独测试程序将指定响应正文扩大到 8 MiB，沿用真实入口、收发函数、事件循环及原始期限，没有修改生产路由，也没有伪造时钟或 EAGAIN。
- 故障组、严重错误组及部分辅助检查显式注入 EINTR 或指定失败；不代表真实信号投递、内存耗尽或资源耗尽测试。
- HTTP 完整性和 EOF/RST 分别记录。有限清理预算不保证任意过量输入都以 FIN 结束，也不保证任意恶意客户端都能读到响应。
- 本轮覆盖当前 LT epoll 学习阶段的约定功能。没有进行长期压力、ThreadSanitizer、优雅退出及未实现协议功能检查。

## 关键源码哈希

完整输入与二进制哈希见最新 JSON 报告。

| 文件 | SHA-256 |
| --- | --- |
| `src/main.cpp` | `da9b9aadf68d4c983ad25e7f1b0dc6d04d5a10d90b599826620bb91076f69ee9` |
| `src/tcp_server.cpp` | `1c9ca53150cc000023faf8316eff4adffed2addbfec6373327f458ec386703e4` |
| `src/tcp_connection.cpp` | `f121f4fbdf1a14bb43e7006bb778c5df06c97b3905c771db87e4037596d8b232` |
| `include/tcp_connection.h` | `c00c5290cdb1774c34835cd6bbd11fcfa176541b131cb76a4194818c8555dfa8` |
| `CMakeLists.txt` | `5b5ff5e1ab1d2d1e3c09c26089d7cbd01b2634e124d16b846624c22d27dd043d` |
| `tests/run_epoll_checks.py` | `aa83dc669bf0ad0d3d5c3589879c80470dc728a48ba5870d5325b8ce2ebffd30` |
| `tests/http_test_support.py` | `d423f73eed290926817e28bb65230ed0d6df25db01b65cb006a1d5c5c1575703` |

本机原始构建、请求、响应及日志目录：`/tmp/cppserver-epoll-checks-8llv33ah`。临时目录不保证长期存在，仓库测试不依赖历史临时文件。
