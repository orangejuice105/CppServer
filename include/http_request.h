#pragma once
#include <string>

//保存解析结果的结构体
struct HttpRequest
{
    std::string method; //方法
    std::string target; //请求目标
    std::string version; //版本
};

//声明解析函数
bool parseRequestLine(const std::string& line,HttpRequest& result); //line: 要解析的请求行，不包含结尾的\r\n
