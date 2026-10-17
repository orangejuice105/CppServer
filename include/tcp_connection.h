#pragma once

//处理一个客户端连接，结束时负责关闭 clientFd。
void handleClient(int clientFd);
