#include "http_router.h"

HttpRouteResult routeRequest(const HttpRequest& request){
    //创建结果对象,并为方法取一个方便使用的名字
    HttpRouteResult result;
    const std::string& method = request.method;

    //请求目标可能包含查询参数/hello?name=Tom ,所以要先找到?再截取
                                              //└─路径─┘└查询参数┘
    std::size_t queryPos = request.pathAndQuery.find("?");
    std::string path = request.pathAndQuery.substr(0,queryPos);

    //检查请求方法，明确服务器允许什么操作
    //认识的方法返回true
    bool knownMethod =
            method == "GET" || method == "HEAD" || method == "POST" ||
            method == "PUT" || method == "DELETE" || method == "CONNECT" ||
            method == "OPTIONS" || method == "TRACE" || method == "PATCH";

    //对不认识的方法处理
    if(!knownMethod){
        result.statusCode = 501; //501表示服务器不认识遇到的方法
        result.reasonPhrase = "Not Implemented";
        result.body = "Not Implemented\n";
    }
    //再处理认识但不允许的方法
    else if(method != "GET"){
        result.statusCode = 405; //405表示该资源不允许这个方法
        result.reasonPhrase = "Method Not Allowed";
        result.body = "Method Not Allowed\n";
    }

    //处理不同路径分支
    else if(path == "/hello"){
        result.body = "Hello,HTTP!\n";
    }

    else if(path == "/health"){
        result.body = "OK\n";
    }

    else {
        result.statusCode = 404;
        result.reasonPhrase = "Not Found";
        result.body = "Not Found\n";
    }

    return result;
}
