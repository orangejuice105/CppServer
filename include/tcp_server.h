#pragma once
#include <signal.h>

//运行LT epoll事件循环，处理监听及客户端事件；失败时释放资源并返回1。
int runTcpServer(int shutdownFd);

//创建包含SIGINT、SIGTERM的信号集合，并把它们屏蔽在当前线程
bool prepareShutdownSignals(sigset_t& signals);
