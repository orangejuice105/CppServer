#pragma once
#include "thread_pool.h"

//运行监听和连接分发流程，使用调用者提供的线程池；失败时返回 1。
int runTcpServer(ThreadPool& pool);
