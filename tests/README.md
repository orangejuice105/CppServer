# 当前 LT epoll 验收

当前版本使用 `run_epoll_checks.py`。旧阻塞式脚本保留作历史记录，不是当前版本的验收入口。

```bash
python3 tests/run_epoll_checks.py --report tests/reports/epoll-latest.json
```

默认运行 16 组检查，在独立临时目录重新构建，开启 `_GLIBCXX_ASSERTIONS` 和 UBSan。
真实 fd 耗尽场景另用独立的 Release + assertions 构建，避免 GCC 11 UBSan 自身申请 pipe 描述符失败干扰判断；不预热 HTTP 响应，保留耗尽状态下首次生成响应的检查。
使用临时回环端口，不占用用户的 8080；只关闭本次测试创建的客户端和服务器。
测试结束保存源码、测试程序、二进制哈希以及收发原始数据、日志和各组结果。

| 分组 | 覆盖范围 |
| --- | --- |
| `regression` | 原有 122 项 HTTP 字节用例，检查状态、响应头、正文、HEAD、分段、EOF、长度限制；逐个响应验证 Date。旧阻塞式 EOF/状态日志要求由响应断言替代，保留当前解析器日志检查 |
| `helpers` | 14 项当前接收、发送、清理和真实 epoll 状态切换检查；包含真实 EAGAIN、部分发送和显式 EINTR 注入 |
| `lifecycle` | 40 次连接、空 EOF、半关闭、真实 TCP RST、端口占用错误日志和 fd 回收 |
| `concurrency` | 80 个不完整请求等待期间处理其他请求；200 次请求、16 个并发客户端；恢复 fd 基线 |
| `faults` | 注入 accept/recv/send/epoll_wait 的 EINTR、收发 EAGAIN、客户端 ADD/MOD/DEL 失败及 shutdown 失败，验证响应、日志和后续服务恢复 |
| `draining` | 客户端保持发送方向打开时的定期清理、持续输入下固定 1 秒期限、64 KiB 预算及资源回收 |
| `oversize` | GET/HEAD 的 4096、4097、8193、16384 字节和 1 MiB 目标，分别一次及分段发送；检查拒绝结果一致和完整响应，单独记录 EOF/RST |
| `fatal-cleanup` | accept 使用 EBADF 模拟致命错误；另覆盖 epoll_wait/监听错误、暂停 DEL/恢复 ADD 失败及清理时 DEL/close 失败；在测试进程退出前检查客户端、监听和 epoll fd 已释放，借用的信号 fd 仍归调用者 |
| `capacity` | 1024 个真实等待连接；额外 101 个连接被拒绝；只在拒绝计数 1、100 时告警；释放一个名额后恢复服务并回收 fd |
| `request-logging` | 合法和格式错误的请求携带人工生成的查询参数、Authorization、Cookie 和其他字段标记，确认日志不输出这些值，保留方法、路径、版本和 fd |
| `graceful-shutdown` | 真实 SIGINT/SIGTERM、已有请求完成、停止接收新连接、重复信号、真实 5 秒退出期限、真实发送背压；显式标记的信号 read/ADD 和监听 DEL 故障注入 |
| `accept-recovery` | 18 项：正常对照、9 种连接/网络错误、EMFILE/ENFILE、连续三次资源不足、暂停期间真实 SIGINT/SIGTERM、暂停 DEL/恢复 ADD 失败，以及子进程真实耗尽 64 个 fd 后释放连接并恢复服务；记录单调时钟重试间隔 |
| `logger-time` | 3 项：正常本地时间、显式注入 localtime_r 失败及随后恢复；调用真实日志队列和析构，检查占位时间、正文和正常日志全部落盘 |
| `receive-timeout` | 真实 30 秒空闲期限；空连接、不完整请求行、GET、HEAD、HTTP/1.0 部分请求头 |
| `request-deadline` | 真实 60 秒固定请求头期限；每隔 20 秒发送片段仍超时，另一个请求在第 50 秒补齐后正常响应 |
| `send-timeout` | 当前 sendResponse 的真实缓冲区停滞与 30 秒空闲期限；第 20 秒恢复一次进展后约第 50 秒超时；两个真实 TCP 发送停滞由生产事件循环扫描释放 |

超时三组并行运行；完整运行通常需要一至两分钟，受构建和机器负载影响。

快速检查或单组选取：

```bash
python3 tests/run_epoll_checks.py --skip-timeouts
python3 tests/run_epoll_checks.py --only send-timeout
```

跳过超时或只选部分组时，报告中的 `full_acceptance` 始终为 `false`。
只有全部 16 组通过、源码和二进制哈希未变化、测试自己的服务器全部停止，才标记为完整验收。

## 测试边界

- HTTP 回归、生命周期、并发、超限以及接收期限使用原始生产 `main`；辅助函数检查调用当前 epoll 实现，旧 `handleClient/sendAll` 不作为当前版本通过依据。
- 正常网络检查仅映射临时端口，不伪造收发返回值。`faults` 及严重错误清理组明确注入故障，不能解释为真实信号或资源耗尽测试。
- `accept-recovery` 的模拟错误与真实资源耗尽分别标记。真实资源耗尽只将一个服务器子进程的 `RLIMIT_NOFILE` 设为 64；等待第二次真实 EMFILE，验证已有请求仍能响应，释放 fd 后等待连接可以接入。测试主进程和用户终端的限制保持原样；ENFILE 使用注入，不耗尽整机资源。
- GCC 11 的 UBSan vptr 检查会申请 pipe；fd 完全耗尽时，检测工具可能把有效的标准库对象报告为无效。已用独立最小程序对比无检测、首次执行、预热和释放两个 fd 的行为，记录见 [诊断报告](reports/ubsan-fd-diagnostic-2026-11-15.json)。真实耗尽使用未插入 UBSan 的生产 Release 构建，其余检查保留 UBSan；没有全局关闭检测或忽略 stderr。
- `logger-time` 仅通过测试目标的链接包装令 localtime_r 返回空指针，不更改系统时间，也不将该包装加入生产服务器。测试进程正常退出后检查文件，验证后台日志已完成落盘。
- 生产路由响应很短，通常不会填满发送缓冲区。发送专项使用单独的测试可执行文件，将带 `X-Test-Large: 1` 的 `/hello` 响应正文扩大为 8 MiB，沿用真实入口、收发函数、事件循环和原来的 30 秒期限。生产路由和源码保持不变；没有伪造时钟或 EAGAIN。
- socketpair 长时间检查也使用真实内核发送缓冲区；第 20 秒读取一批字节，验证发送进展能延后期限，后续 EAGAIN 不延后期限。
- 有界清理不能保证任意超量输入都以 FIN 结束。超限组始终要求完整 HTTP 响应，另行记录 EOF/RST；不声称任意恶意客户端都一定能收到完整响应。
- fd 检查、UBSan 和这些用例不等于证明所有路径无泄漏、无内存错误；未进行 ThreadSanitizer、长期压力测试或尚未实现的协议功能检查。
- 当前仍采用约 100 毫秒轮询扫描，截止时间处理会受调度和当前批次处理耗时影响。
- 停止接收阶段只从 epoll 删除监听登记，监听 fd 在退出时关闭；这期间 TCP 握手仍可能成功，验收要求新连接不再被应用接收和处理。
- `graceful-shutdown` 的 8 MiB 响应仅用于真实发送背压。到达退出期限后允许未完成响应被截断，并要求记录剩余连接数量；这与期限内完成的完整响应分别检查。

2026-11-15 的 16 组完整验收见 [RECOVERY_BASELINE.md](RECOVERY_BASELINE.md)，原始报告为 [reports/epoll-recovery-2026-11-15.json](reports/epoll-recovery-2026-11-15.json)。2026-11-14 的 14 组记录见 [HARDENING_BASELINE.md](HARDENING_BASELINE.md)，基础 epoll 历史验收见 [EPOLL_BASELINE.md](EPOLL_BASELINE.md)；各次结果只对应各自报告中的输入哈希。

## 独立性能测试

```bash
python3 tests/run_epoll_benchmark.py --report tests/reports/epoll-performance-latest.json
```

脚本创建独立 Release 构建，默认对 `/health` 和 `/hello` 分别运行 1、16、64 并发短连接；每种场景预热 0.5 秒，测量 3 秒，重复两次。额外覆盖 256 个等待连接下的正常请求，以及 1024 个名额已满时的拒绝行为。可用 `--duration`、`--warmup`、`--repeats`、`--concurrency` 调整参数。

2026-11-15 的本机重测见 [RECOVERY_BASELINE.md](RECOVERY_BASELINE.md)，完整测量见 [reports/epoll-performance-2026-11-15.json](reports/epoll-performance-2026-11-15.json)。2026-11-14 的历史结果保留在 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。

逐次核对 200 状态、响应头、正文及发送方向关闭；每个请求重新建立 TCP 连接。延迟从发起连接计至读完响应和本地连接清理；吞吐量只统计完整成功响应，并计入最后一批在途请求的完成时间。满容量场景的 EOF/RST 单独统计，不算成功请求。

服务器保持生产 INFO 日志配置、backlog、收发和关闭策略。`epoll_observed_server` 与正式入口共用生产代码，仅在事件循环返回后导出 `LoggerStats`；测试 shim 只映射端口、累计 accept 成功次数，性能运行不启用任何故障注入。日志队列丢弃、日志文件保留情况及连接拒绝数量单独记录，不通过关闭日志提高数字。

报告保存每次测量的成功请求数和 QPS、p50/p95/p99 延迟、错误分类、服务器与客户端 CPU、RSS/fd 峰值和恢复情况、日志丢弃及队列峰值、环境和输入哈希。客户端与服务器同机运行，Python 发压端可能先达到瓶颈；短时本机结果不等于生产容量。性能结果以生成的报告为准，不预先承诺 QPS。

结果解读要求：

- 使用独立 Release 构建，保持实际生产路由、超时和连接关闭策略，固定并记录日志级别与输出方式；功能验收中的 UBSan 构建不用于性能结论。
- 覆盖 `/hello`、`/health` 的逐级并发短连接请求，以及慢速或空闲连接占用期间的正常请求。按当前每连接一次请求的行为发压，不假定支持 keep-alive；明确请求延迟的计时范围。
- 预热后进行固定时长的测量并重复运行，记录成功请求吞吐量、p50/p95/p99 延迟、HTTP 状态和响应完整性、超时及连接错误、容量拒绝数、服务器 CPU、RSS 和 fd 数；检查结束后资源恢复情况，并单独记录日志丢弃情况。
- 保存可重复运行的脚本、源码提交和工作区改动状态、构建参数、机器与系统配置、文件描述符限制、日志配置、并发数、测量时长和原始结果。正常负载和超过容量的负载分别报告，不把预期容量拒绝计为成功吞吐量。
- 记录客户端负载与运行位置；客户端和服务器同机、回环网络或发压端达到瓶颈时明确结果限制。使用实测结果建立基线，不预先承诺固定 QPS，也不据单次本机测试宣称生产环境性能。


## 历史阶段

旧 Echo、仅接收请求头及阻塞式 HTTP 脚本和辅助程序已移至 [history/](history/README.md)，不参与当前构建。旧报告和阶段记录保留供查阅；复现时使用对应的历史 Git 提交。当前验收已独立使用 `http_test_support.py`，不依赖旧脚本。
