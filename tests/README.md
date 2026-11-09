# 当前 LT epoll 验收

当前版本使用 `run_epoll_checks.py`。旧阻塞式脚本保留作历史记录，不是当前版本的验收入口。

```bash
python3 tests/run_epoll_checks.py --report tests/reports/epoll-latest.json
```

默认运行 11 组检查，在独立临时目录重新构建，开启 `_GLIBCXX_ASSERTIONS` 和 UBSan。
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
| `fatal-cleanup` | 注入三种服务器级严重错误，以及清理时 DEL/close 失败；在测试进程退出前检查所有客户端、监听和 epoll fd 已释放 |
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
只有全部 11 组通过、源码和二进制哈希未变化、测试自己的服务器全部停止，才标记为完整验收。

## 测试边界

- HTTP 回归、生命周期、并发、超限以及接收期限使用原始生产 `main`；辅助函数检查调用当前 epoll 实现，旧 `handleClient/sendAll` 不作为当前版本通过依据。
- 正常网络检查仅映射临时端口，不伪造收发返回值。`faults` 及严重错误清理组明确注入故障，不能解释为真实信号或资源耗尽测试。
- 生产路由响应很短，通常不会填满发送缓冲区。发送专项使用单独的测试可执行文件，将带 `X-Test-Large: 1` 的 `/hello` 响应正文扩大为 8 MiB，沿用真实入口、收发函数、事件循环和原来的 30 秒期限。生产路由和源码保持不变；没有伪造时钟或 EAGAIN。
- socketpair 长时间检查也使用真实内核发送缓冲区；第 20 秒读取一批字节，验证发送进展能延后期限，后续 EAGAIN 不延后期限。
- 有界清理不能保证任意超量输入都以 FIN 结束。超限组始终要求完整 HTTP 响应，另行记录 EOF/RST；不声称任意恶意客户端都一定能收到完整响应。
- fd 检查、UBSan 和这些用例不等于证明所有路径无泄漏、无内存错误；未进行 ThreadSanitizer、长期压力测试、优雅退出或尚未实现的协议功能检查。
- 当前仍采用约 100 毫秒轮询扫描，截止时间处理会受调度和当前批次处理耗时影响。

验收结论与具体源码哈希见 [EPOLL_BASELINE.md](EPOLL_BASELINE.md)。


## 历史阶段

旧 Echo、仅接收请求头及阻塞式 HTTP 脚本和辅助程序已移至 [history/](history/README.md)，不参与当前构建。旧报告和阶段记录保留供查阅；复现时使用对应的历史 Git 提交。当前验收已独立使用 `http_test_support.py`，不依赖旧脚本。
