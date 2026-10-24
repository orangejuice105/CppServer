# CppServer：阻塞式 HTTP 学习项目

使用 C++17、Linux socket API、固定线程池和异步日志库构建的基础 HTTP 服务器。
监听 `127.0.0.1:8080`，提供 GET 文本响应，每个连接处理一次请求后关闭。

本阶段范围是可运行、可重复验证的阻塞式多线程 HTTP 基础版本，不是完整的 HTTP/1.1 实现。

## 构建与运行

需要 Linux / WSL、C++17 编译器、CMake 3.13 或更新版本及系统线程库：

```bash
cmake -S . -B build
cmake --build build -j 4
./build/main
```

另开终端验证：

```bash
curl --http1.1 -i --max-time 3 http://127.0.0.1:8080/hello
curl --http1.1 -i --max-time 3 http://127.0.0.1:8080/health
curl --http1.1 -i --max-time 3 http://127.0.0.1:8080/missing
curl --http1.1 -i --max-time 3 'http://127.0.0.1:8080/hello?name=Tom'
```

| 路径 | 状态 | 正文 | 正文字节数 |
|---|---|---|---|
| `/hello` | 200 OK | `Hello,HTTP!` 加换行 | 12 |
| `/health` | 200 OK | `OK` 加换行 | 3 |
| `/missing` | 404 Not Found | `Not Found` 加换行 | 10 |
| `/hello?name=Tom` | 200 OK | `Hello,HTTP!` 加换行 | 12 |

在服务器终端按 `Ctrl+C` 停止进程。当前没有优雅退出机制；操作系统会回收资源，
但异步日志队列中尚未写入文件的消息不保证保存。

## 默认配置

| 配置 | 当前值 | 位置 |
|---|---|---|
| 监听地址 | `127.0.0.1:8080` | `src/tcp_server.cpp` |
| 工作线程 | 4 | `src/main.cpp` |
| 线程池等待队列容量 | 64 | `src/main.cpp` |
| 接收、发送超时 | 各 30 秒 | `src/tcp_connection.cpp` |
| 请求头总上限 | 8192 字节，包含请求行与结束空行 | `src/tcp_connection.cpp` |
| 请求目标上限 | 4096 字节，包含查询参数 | `src/tcp_connection.cpp` |
| 请求正文长度上限 | 0 | `src/tcp_connection.cpp` |
| 日志最低级别 | INFO | `src/main.cpp` |

队列容量只计算尚未被工作线程取走的任务，正在处理的连接不占这 64 个位置。
任务队列满时关闭新接收的连接并记录错误。它与 `listen()` 的操作系统连接等待队列不同。
主线程负责 `accept()`，工作线程负责一个连接的收发及关闭；阻塞收发会占用对应工作线程。

30 秒超时在工作线程开始处理连接时设置，限制单次阻塞收发操作；它不覆盖排队时间，
也不是整个请求或连接的总时限。连接收尾另使用短超时和总预算。

## 已实现行为

- `socket → bind → listen → accept`；绑定前设置 `SO_REUSEADDR`。
- 累积分段数据，以 `\r\n\r\n` 判断请求头结束，不把一次 `recv()` 当作完整请求。
- 解析方法、原始目标和版本；校验方法 token、目标字符、百分号编码和目标形式。
- 解析普通路径、http 绝对 URI、CONNECT 主机与端口及 OPTIONS 星号目标；路由使用路径并去掉查询参数。
- 解析请求头，字段名转小写，去掉值两端的空格和制表符，保存顺序及重复字段。
- 校验 Host 的数量和内容，包含端口、reg-name、方括号 IPv6 等情况。
- HTTP/1.1 要求一个 Host，HTTP/1.0 允许省略；重复 Host 拒绝。
- Content-Length 必须为数字且不重复；合法非零正文长度返回 413。
- 解析 Transfer-Encoding 的列表、参数、引号与转义；要求 chunked 唯一且位于末尾。
  与 Content-Length 同时出现或用于 HTTP/1.0 时拒绝；通过语法检查后仍返回 501，因为没有正文解码器。
- 仅允许 GET；已知但不允许的方法返回 405 和 `Allow: GET`，未知合法方法返回 501。
  HEAD 的错误响应不发送正文，当前也省略 Content-Length。
- 接受语法合法的 HTTP/1.x；HTTP/1.0 使用 1.0 响应，其余使用 1.1 响应；其他主版本返回 505。
- 解析失败返回 400，未知路径返回 404。目标超限返回 414，已能确认时在请求头完整之前拒绝。
- 请求头超限时，若已有完整、可解析且未超头部预算的请求行，尝试发送 431。
  接收超时或客户端提前结束发送时，同样按请求行条件尝试发送 408 或 400；空连接直接关闭。
- 目标超限但请求行尚未完成时使用 HTTP/1.1 错误响应，能识别 HEAD 时省略正文。
- 独立响应模块生成状态行、Date、Content-Type、Content-Length 和 `Connection: close`。
  正文长度按字节计算；Date 为英文 UTC 日期，生成失败时省略。
- `sendAll()` 处理部分发送、EINTR、发送超时及其他失败，使用 `MSG_NOSIGNAL`。
- 收尾先 `shutdown(SHUT_WR)`，再有界清理剩余输入，最后 `close()`。
  清理最多 64 KiB，单次接收等待 100 ms，循环总预算约 1 秒；预算检查后的一次接收可再等待最多约 100 ms。
  这减少带未读输入直接关闭时的复位风险，不保证任意恶意发送场景都不会发生 TCP 复位。
- 异步记录启动、连接、请求、响应及错误；系统调用失败保存 errno，日志支持按大小滚动。

## 重复运行验收

除构建工具外，只需 Python 3 标准库。在项目目录执行：

```bash
python3 tests/run_http_checks.py
```

默认执行全套检查，包括真实 30 秒接收及发送超时。两项超时并行等待，实际总耗时还包含构建。
测试使用临时回环端口，不要求停止你在 8080 上运行的服务；只终止脚本自己启动的进程。
报告和原始输出默认保存在脚本打印的临时目录。

保存报告、快速检查或单独运行一组：

```bash
python3 tests/run_http_checks.py --report tests/reports/http-latest.json
python3 tests/run_http_checks.py --skip-timeouts
python3 tests/run_http_checks.py --only concurrency
```

跳过超时或只选部分组不算完整验收。详细覆盖范围、隔离方式和限制见
[tests/README.md](tests/README.md)，最终验收记录见 [tests/HTTP_BASELINE.md](tests/HTTP_BASELINE.md)。
旧 Echo 与只接收请求头的脚本保留作历史资料，不能用于验证当前服务器。

## 模块结构

| 模块 | 职责 |
|---|---|
| `src/main.cpp` | 初始化日志、创建线程池、调用服务器入口 |
| `include/tcp_server.h`、`src/tcp_server.cpp` | 建立监听、接收连接、向线程池提交任务；提交失败关闭新连接 |
| `include/tcp_connection.h`、`src/tcp_connection.cpp` | 单连接超时、接收、调用解析与路由、发送以及关闭 |
| `include/http_request.h`、`src/http_request.cpp` | 请求结构、请求行和字段解析、Host 与长度及传输编码校验 |
| `include/http_router.h`、`src/http_router.cpp` | 方法与路径选择，返回路由结果 |
| `include/http_response.h`、`src/http_response.cpp` | HTTP 日期、状态行、响应头及正文组装 |
| `include/thread_pool.h`、`src/thread_pool.cpp` | 固定工作线程、有界任务队列 |
| `include/logger.h`、`src/logger.cpp` | 异步日志及文件滚动 |
| `tests/run_http_checks.py` | 当前阶段可重复运行的验收入口 |
| `tests/http_cases.json` | 原始 HTTP 字节用例和预期响应 |
| `tests/http_connection_checks.cpp` | 真实连接辅助函数检查及发送超时探针 |
| `tests/socket_test_shim.cpp` | 仅测试使用的端口映射和中断、部分发送注入 |

## 查看日志

日志相对于进程启动目录写入 `server.log`。从项目目录运行 `./build/main` 后：

```bash
tail -n 30 server.log
```

若先进入 `build` 再执行 `./main`，则查看 `build/server.log`。
日志附带时间、级别、线程编号、源码位置，连接事件带有 fd。fd 会被复用，不能当作永久连接身份。
Logger 初始化失败输出到终端；日志队列满会丢弃新消息。

## 阶段边界

当前不接收非零长度请求正文，不解码 chunked，不支持成功的 HEAD、keep-alive、
pipelining 或静态文件服务；每个连接只处理第一个请求。没有完整请求总时限、排队超时、
优雅退出和长期压力测试结果。`accept()` 的非 EINTR 错误会结束接收循环并等待已提交任务完成。

这些扩展可安排在后续阶段；本阶段以已列出的 GET 文本服务、错误处理、连接边界及可重复验收为交付范围。
