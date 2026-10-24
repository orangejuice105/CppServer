# 阻塞式 HTTP 验收说明

## 运行

需要 Linux / WSL、Python 3、C++17 编译器、CMake 3.13+，不依赖第三方 Python 包。
从仓库根目录运行：

```bash
python3 tests/run_http_checks.py
```

默认新建临时构建目录，启用断言与未定义行为检测，构建服务器和测试工具，运行全部 7 组检查。
接收、发送超时各实际等待约 30 秒，两组并行执行，正式超时配置不缩短。
正常服务器构建默认不包含测试探针；验收脚本通过 `CPPSERVER_BUILD_TEST_TOOLS=ON` 启用它。

```bash
python3 tests/run_http_checks.py --report tests/reports/http-latest.json
python3 tests/run_http_checks.py --skip-timeouts
python3 tests/run_http_checks.py --only regression
python3 tests/run_http_checks.py --only receive-timeout --only send-timeout
```

`--only` 可以重复。`--skip-timeouts` 或部分分组报告中的 `full_acceptance` 为 false；
只有全 7 组通过、源码未变、自己启动的服务器全部停止时才为 true。
任何检查失败时脚本返回非零状态码。默认报告位于打印出的 `ARTIFACTS` 目录；
`--report` 另外保存一份 JSON，不会把临时目录中的可执行文件加入仓库。

## 覆盖范围

| 分组 | 内容 |
|---|---|
| `regression` | 122 个原始字节 HTTP 用例：路由、方法、HEAD、版本、Host、Content-Length、Transfer-Encoding、畸形请求、分段接收、EOF、8192 字节头部和 4096 字节目标边界、提前拒绝、缓冲输入及后续恢复 |
| `helpers` | 16 项请求目标前缀检查；9 项连接收尾检查，包括空闲、EOF、32769 字节清理、64 KiB 上限、持续少量输入下的时间预算，以及注入的 EINTR/关闭发送方向失败/超时设置失败/接收错误 |
| `lifecycle` | 连续 40 次请求、半关闭、客户端 RST、日志、描述符恢复、第二服务器端口冲突退出 1，原服务器继续响应 |
| `concurrency` | 三个未完成请求时第四个正常响应；确认四个工作线程；第五个任务排队；4 个执行加 64 个等待后拒绝额外连接；已接收任务全部完成；16 个并发客户端发送 200 次请求并检查资源恢复 |
| `faults` | accept/recv 的 EINTR 各 1 次、send 的 EINTR 2 次；每次真实发送最多 17 字节；完整响应无丢失或重复 |
| `receive-timeout` | 原配置 4 个线程同时处理：空连接、GET 部分头、HEAD 部分头、HTTP/1.0 部分头；真实等待 30 秒，分别检查无响应关闭或 408、版本、HEAD 无正文、日志和恢复 |
| `send-timeout` | 直接调用真实生产 `sendAll()`，填满真实 AF_UNIX stream socketpair 的内核发送缓冲区，对端不读，设置 30 秒发送超时，检查实际等待、失败返回和 WARN 日志 |

HTTP 用例检查完整状态行、响应字段、正文和连接 EOF，TCP 复位不会被忽略。
每个 HTTP 响应单独验证 Date 格式、英文星期月份、实际 UTC 时间；回归服务器在纽约时区运行。
动态 Date 不参与静态响应字节比较，其余字段和正文必须符合用例。

## 隔离方式与证据

- 使用仓库当前 CMake 配置构建原始生产源码，保持 `pool(4,64)` 及 30 秒超时。
- 测试专用 `LD_PRELOAD` 辅助库只在子进程中把 `127.0.0.1:8080` 的 bind 映射到临时回环端口。
  因此不要求释放 8080，也不改写 `tcp_server.cpp`。生产日志仍显示代码中的 8080，测试客户端连接映射后的临时端口。
- 仅在 `faults` 组设置中断与发送大小限制，其他网络分组不注入这些行为。
- 清理辅助函数通过链接包装器精确注入指定失败，正常字节和时间边界使用真实 socket I/O。
- 日志线程或检测工具可能立即复用刚关闭的 fd 数字，辅助检查比较原 socket 身份并确认 close 返回成功，避免把编号复用误判为泄漏。
- 每个服务器的标准输出、标准错误、日志与辅助库事件，HTTP 原始收发字节，构建输出及辅助探针输出均保存在临时目录。
- 报告记录源码、测试代码、用例及可执行文件的 SHA-256；测试前后必须一致。
- 脚本只关闭自己创建的客户端和服务器，不停止用户已有服务。

JSON 报告随仓库保存；原始输出目录是本机临时目录，不保证长期存在。
测试脚本与用例已入仓库，换机后可以重新运行，不再依赖此前 `/tmp` 中的临时脚本或历史报告作为输入。

## 验证限制

发送超时测试覆盖真实生产 `sendAll()` 和真实内核阻塞；它不是大响应 HTTP 路由测试，
因为当前路由只有很小的固定文本。没有新增测试专用大响应路由，也没有伪造超时返回。

EINTR 来自辅助库/链接器注入，没有模拟真实信号投递或 SA_RESTART 行为。
资源回收检查覆盖这些测试场景，不表示证明了所有路径或长期运行都无泄漏。
未定义行为检测无诊断不等于排除所有内存和并发问题，未运行 ThreadSanitizer 或长期压力测试。
有界清理不保证任意持续发送的客户端都不会触发复位。当前不测未实现的正文解码、长连接和优雅退出。

## 历史材料

- `run_tcp_checks.py`、`BASELINE.md`：旧 TCP Echo 阶段，要求旧的 `(1,1)` 配置并期待回显，不能用于当前 HTTP 服务器。
- `run_http_header_checks.py`、`HTTP_HEADER_BASELINE.md`：仅接收、打印请求头的旧阶段，不能用于当前会发送 HTTP 响应的版本。
- `reports/` 中早期 JSON：记录当时源码和测试范围，不自动代表当前版本已重新通过。
- 当前完整验收记录：[HTTP_BASELINE.md](HTTP_BASELINE.md)。
