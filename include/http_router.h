//根据方法和路径选择结果
#pragma once
#include <string>
#include "http_request.h"

//保存分发结果
struct HttpRouteResult
{
    int statusCode = 200; //状态码
    std::string reasonPhrase = "OK"; //状态说明
    std::string body; //正文
};

//分发函数
HttpRouteResult routeRequest(const HttpRequest& request);