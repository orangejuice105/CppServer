# CppServer：阻塞式 HTTP 学习项目

使用 C++17、Linux socket API、固定线程池和异步日志库，从 TCP Echo 逐步构建
阻塞式 HTTP 服务器的学习项目。目前监听 `127.0.0.1:8080`，支持基础请求行和
请求头字段解析，按方法、版本和路径选择文本响应，每个连接处理一次请求后关闭。

## 当前功能

- 使用 `socket → bind → listen → accept` 建立服务端入口。
- 累积多次接收的数据，通过 `\r\n\r\n` 判断请求头结束，包含结束标记最多 8192 字节。
- 解析请求行的方法、请求目标和版本，拒绝缺项及多余空格等基本结构错误。
- 校验方法 token 和目标字符；解析普通路径、http 绝对 URI、CONNECT 主机与端口及 OPTIONS 星号目标。
- 校验版本字符串格式；接受 HTTP/1.x，对其他主版本返回 505；HTTP/1.0 请求使用 HTTP/1.0 响应，其余使用 HTTP/1.1 响应。
- 解析全部请求头字段并保存到 `HttpRequest::headers`：检查字段名和字段值字符，字段名转为小写，去掉值两端的空格和制表符，保留字段顺序及重复字段。
- 校验 Host 的数量和值；HTTP/1.1 必须包含一个 Host，HTTP/1.0 允许省略，支持方括号 IPv6 地址。
- 校验 Content-Length；拒绝重复或非数字的值，当前正文长度上限为 0，非零长度返回 413。
- 解析 Transfer-Encoding 的有序列表、参数和引号值，检查 chunked 必须唯一且位于末尾；拒绝与 Content-Length 同时出现及 HTTP/1.0 使用传输编码。
- 通过传输编码检查的请求暂时返回 501，因为当前尚未实现请求正文解码。
- 请求行或字段解析失败时返回 400；已知但不允许的方法返回 405 并附带 `Allow: GET`，未知方法返回 501。
- 当前仅允许 GET；HEAD 返回错误响应时省略正文和 Content-Length。
- 分离查询参数后匹配路径：`/hello`、`/health` 返回 200，未知路径返回 404。
- 由独立响应模块构造状态行、Date、Content-Type、Content-Length 和 Connection: close，正文长度按字节计算。
- Date 使用 UTC、英文星期和月份及 GMT 标记，不受本地时区和全局 C++ 语言环境影响；时间获取或格式化失败时省略该字段。
- `sendAll()` 记录发送进度，处理部分发送、中断、发送超时和其他发送错误。
- 使用 `MSG_NOSIGNAL`，避免发送时产生 SIGPIPE 信号。
- 主线程持续接收连接，工作线程处理请求；发送完成或失败后关闭该连接。
- 连接收发与关闭集中在 `handleClient()` 函数中。
- 线程池等待队列有容量上限，在同一把锁内检查容量并入队；满时抛出异常，服务器捕获后关闭新连接。
- 为连接设置接收和发送超时，区分超时与其他收发错误。
- 在绑定前启用 `SO_REUSEADDR`，方便服务器重启时按系统规则复用地址。
- 异步记录启动成功与失败、连接接收、处理开始和结束、收发超时和错误，以及超时设置与任务提交失败。
- 日志附带时间、级别、提交线程编号和源码位置，连接事件带有 fd，支持按大小滚动。

## 当前配置

当前保留了便于观察排队和拒绝行为的测试配置：

- `src/main.cpp` 中的 `ThreadPool pool(1, 1)`：1 个工作线程，最多 1 个任务等待执行。
- `src/tcp_connection.cpp` 中的接收、发送超时均为 30 秒，在 `handleClient()` 开始处理连接时设置。
- 日志最低级别为 INFO，追加写入启动目录下的 `server.log`。

队列容量只计算尚未被工作线程取走的任务，不包含正在执行的任务。
增加工作线程数量后，可以同时处理多个客户端；当前测试配置一次处理一个客户端。
超时限制单次阻塞收发操作，不限制连接总存活时间，也不覆盖在线程池队列中等待的时间。

## 构建与运行

面向 Linux / WSL，需要支持 C++17 的编译器和 CMake 3.13 或更新版本。
在本项目目录执行：

```bash
cmake -S . -B build
cmake --build build
./build/main
```

## HTTP 手动验证

保留运行服务器的终端，在同一个 Linux / WSL 环境中新开一个终端执行：

```bash
curl --http1.1 -i --max-time 3 http://127.0.0.1:8080/hello
curl --http1.1 -i --max-time 3 http://127.0.0.1:8080/health
curl --http1.1 -i --max-time 3 http://127.0.0.1:8080/missing
curl --http1.1 -i --max-time 3 'http://127.0.0.1:8080/hello?name=Tom'
```

| 路径 | 状态 | 正文 | Content-Length |
|---|---|---|---|
| `/hello` | 200 OK | `Hello,HTTP!` 加换行 | 12 |
| `/health` | 200 OK | `OK` 加换行 | 3 |
| `/missing` | 404 Not Found | `Not Found` 加换行 | 10 |
| `/hello?name=Tom` | 200 OK | `Hello,HTTP!` 加换行 | 12 |

正文之后连接关闭。收到 404 后，再请求 `/health` 应仍正常。请求行和头字段解析成功时，
日志记录方法、完整目标、版本和各字段，例如 `方法=GET,目标=/hello,版本=HTTP/1.1`
以及 `请求头:host=127.0.0.1:8080`。

在服务器终端按 `Ctrl+C` 可以停止进程，此时资源由操作系统回收。
当前没有主动结束主循环的优雅退出机制。

## 验证记录

2026-10-22 当前源码通过 CMake 构建、4 项日期生成检查和 41 项临时端口 HTTP 检查。
日期检查覆盖 UTC、上海、纽约时区及自定义非英文全局 C++ 语言环境；每项均校验格式和实际 UTC 时间。
HTTP 检查覆盖常用路径、方法、HEAD、版本、Content-Length、Transfer-Encoding 及分次发送。
全部响应均含一个格式正确且时间有效的 Date，去掉该字段后与此前验证过的响应字节一致。
检查启用未定义行为检测，未发现诊断；本轮未重跑真实 30 秒收发超时、并发、队列满及描述符回收检查。
报告见 [tests/reports/http-transfer-date-2026-10-22.json](tests/reports/http-transfer-date-2026-10-22.json)。
本轮工具和原始输出保留在报告对应的本机临时目录，报告不代表完整阶段验收。

2026-10-19 本轮 CMake 构建、20 项整块请求头解析函数检查和 49 项临时端口 HTTP
检查通过。网络检查覆盖路径、方法、版本、错误响应、头字段规范化、重复字段保留、
非法字段拒绝、日志及分次发送。函数检查包含缺少结束空行的失败路径，并启用未定义
行为检测。本轮未重新执行真实 30 秒收发超时等完整网络边界检查。
记录见 [tests/reports/http-request-2026-10-19.json](tests/reports/http-request-2026-10-19.json)。
本轮检查工具和原始输出位于本机临时目录，未加入仓库，报告不代表完整阶段验收。

历史 TCP Echo 完整 11 项检查见 [tests/BASELINE.md](tests/BASELINE.md)；
仅接收和打印请求头时的 16 项检查见
[tests/HTTP_HEADER_BASELINE.md](tests/HTTP_HEADER_BASELINE.md)。完整历史报告
保存在 `tests/reports/`。这些结果对应当时源码，不代表当前版本完成了同等全套回归。
旧脚本的预期行为与当前 HTTP 响应不同，适用范围见 [tests/README.md](tests/README.md)。

## 查看日志

按上面的方式在本项目目录启动后，可在另一个终端查看：

```bash
cat server.log
```

若进入 `build` 后执行 `./main`，日志则在 `build/server.log`。
日志路径相对于进程启动时的工作目录，与可执行文件所在位置无直接对应关系。

INFO 记录连接生命周期，WARN 记录收发超时，ERROR 记录其他收发错误、设置
超时失败和任务提交失败。系统调用错误带有保存的 errno 编号和文字原因，
任务提交异常使用 `e.what()` 记录原因。启动阶段及 `accept()` 返回 `-1` 的
失败分支也会写入 ERROR 日志；Logger 自身初始化失败仍输出到终端。

可通过日志观察主线程接收 A、B、C，工作线程处理 A，B 排队，C 被拒绝，
随后工作线程结束 A 并开始处理 B。fd 可能被系统复用，不是永久连接身份。

日志队列满时会丢弃新日志；正常退出时单例析构会等待后台消费剩余消息，
`Ctrl+C` 等直接终止进程的方式不保证尚在队列中的日志写完。生成的日志及
归档文件已加入 Git 忽略规则。

## 项目结构

```text
project3/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── include/
│   ├── thread_pool.h
│   ├── tcp_connection.h
│   ├── tcp_server.h
│   ├── http_request.h
│   ├── http_response.h
│   └── logger.h
├── src/
│   ├── main.cpp
│   ├── tcp_connection.cpp
│   ├── tcp_server.cpp
│   ├── http_request.cpp
│   ├── http_response.cpp
│   ├── thread_pool.cpp
│   └── logger.cpp
└── tests/
    ├── run_tcp_checks.py
    ├── run_http_header_checks.py
    ├── socket_test_shim.cpp
    ├── README.md
    ├── BASELINE.md
    ├── HTTP_HEADER_BASELINE.md
    └── reports/
```

`tcp_connection.h` 声明连接处理入口，`tcp_connection.cpp` 负责单个连接的超时、
接收、路由、响应发送和关闭。`http_request.h/.cpp` 保存请求行及字段结果，完成请求目标、
Host、Content-Length 和 Transfer-Encoding 的解析与校验。
`http_response.h/.cpp` 负责生成 HTTP 日期并组装完整响应。
`tcp_server.h/.cpp` 提供 `runTcpServer()`，负责建立监听、接收
连接并向线程池提交任务。`main.cpp` 负责初始化日志、创建线程池并调用服务器入口。

## 当前边界与后续方向

仅监听本机回环地址；`accept()` 失败仍会停止主线程接收连接，线程池析构会等待
已接受任务完成。`accept/recv/send` 已处理 `EINTR` 重试，目前没有优雅退出机制。
阻塞收发期间会占用工作线程，超时不是整个连接或请求的总时限。
当前只提供 GET 文本响应，不接收非零长度正文；合法的传输编码请求暂时返回 501，
尚未实现 chunked 正文解码，因此还不能称为完整的 HTTP/1.1 实现。不支持 keep-alive；
头部超长或接收超时仍直接关闭连接，尚未返回对应 HTTP 错误。
后续继续完善错误响应、拆分路径处理模块并完成阻塞式 HTTP 阶段验收，再学习非阻塞 I/O。
