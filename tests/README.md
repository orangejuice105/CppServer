# TCP 阶段验证

面向 Linux / WSL，使用 Python 3 标准库、g++ 和本机回环地址。
主程序保持 `ThreadPool pool(1,1)` 和 30 秒收发超时，脚本会检查这两个条件。
运行前请停止已有的 8080 服务；脚本不会终止用户已有进程。

在项目目录执行：

```bash
cmake -S . -B build
cmake --build build
python3 tests/run_tcp_checks.py
```

完整检查包含两次真实的 30 秒超时，总计约一分钟以上。
每项使用独立日志目录，终端打印 PASS/FAIL 和结果文件路径。
测试退出时清理自己启动的进程，保留临时文件便于查看。
程序有本机网络权限限制时，需要先允许这次测试。

仅运行指定检查，可以重复传入 `--case`：

```bash
python3 tests/run_tcp_checks.py --case basic --case large
python3 tests/run_tcp_checks.py --case overload
```

| 检查 | 方法与判定 |
|---|---|
| basic | 同一连接连续收发 UTF-8、含零字节的二进制和分段数据，比较实际字节 |
| large | 发送 256 KiB，循环接收并比较完整内容，覆盖多次 recv/send |
| half_close | 客户端结束发送方向，继续读取完整回显和服务端 EOF |
| overload | A 完成回显后保持连接，B 排队，C 被拒绝；A 结束后 B 得到回显 |
| reconnect | 连续连接 40 次，检查正常路径文件描述符数回到基线 |
| reset | 客户端用 SO_LINGER 触发 RST，检查错误日志和后续服务 |
| bind_conflict | 第二进程绑定失败、返回 1 并写入独立错误日志，第一进程继续回显 |
| multi | 编译仅把配置改成 (4,4) 的临时源码副本，保持 4 个连接并验证 4 个工作线程回显 |
| injected | 注入 accept/recv/send 的 EINTR，并限制每次实际发送量，检查数据无丢失、无重复 |
| receive_timeout | 原配置 30 秒，客户端不发送；检查关闭、WARN 日志和后续服务 |
| send_timeout | 缩小测试连接的发送缓冲区，客户端发送但不读响应；检查真实发送超时和后续服务 |

## 测试辅助库的作用

`socket_test_shim.cpp` 只为验证使用，不加入正常 CMake 构建。
脚本临时编译它，通过 `LD_PRELOAD` 仅加载到指定的测试服务器进程中。

- `injected`：accept、recv 各返回一次 `-1/EINTR`，send 在第 1 和第 3 次调用
  返回 `-1/EINTR`，覆盖零进度和已有部分发送进度的重试。其他 send 调用最多
  向真实系统接口提交 17 字节，返回实际发送数量，验证服务器的偏移计算。
- `send_timeout`：只缩小服务器连接的发送缓冲区，不伪造超时返回值；客户端
  不读响应，使真实阻塞 send 达到原配置超时。

错误注入证明相应重试分支能工作，不等于验证了真实信号的投递、处理函数或
自动重启语义。分批发送验证也不代表已经穷举内核产生部分发送的各种条件。

临时的四线程副本只更改线程池配置，不修改项目源码或正式可执行文件。
检查结果不代表长时间压测、所有异常路径的资源安全或优雅退出已完成。

结果见 [BASELINE.md](BASELINE.md)。
