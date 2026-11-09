#pragma once

//运行LT epoll事件循环，处理监听及客户端事件；失败时释放资源并返回1。
int runTcpServer();
