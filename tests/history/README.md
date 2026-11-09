# 历史阶段测试归档

此目录保存 TCP Echo、只接收请求头及阻塞式多线程 HTTP 阶段的测试源码。它们依赖相应阶段的入口、日志及函数，不参与当前 CMake 构建，也不能直接验证当前 epoll 版本。

当前入口：

```bash
python3 tests/run_epoll_checks.py
```

如需复现历史阶段，请在单独的检出目录中使用匹配的 Git 提交及其原有测试文件：

- `e2a5e46`：基础阻塞式 HTTP 服务与完整验收。
- `0d2a0b1`：固定请求头总时限和超限输入验收。

旧报告、`tests/BASELINE.md`、`tests/HTTP_HEADER_BASELINE.md` 和 `tests/HTTP_BASELINE.md` 均记录当时的代码与覆盖范围，不能替代当前验收。历史报告中的路径及哈希按原记录保留。
