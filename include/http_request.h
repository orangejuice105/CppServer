#pragma once
#include <string>
#include <vector>

//保存一行请求头结果的结构体
/*
Host: 127.0.0.1:8080
字段名 name：Host
字段值 value：127.0.0.1:8080
*/
struct HttpHeader
{
    std::string name;
    std::string value;
};

//保存解析结果的结构体
struct HttpRequest
{
    std::string method; //方法
    std::string target; //请求目标
    std::string version; //版本
    std::vector<HttpHeader> headers;
};




//声明解析函数
bool parseRequestLine(const std::string& line,HttpRequest& result); //line: 要解析的请求行，不包含结尾的\r\n

//声明解析一行请求头函数
bool parseHeaderLine(const std::string& line,HttpHeader& result);

//声明解析整块请求头的函数
bool parseRequestHeaders(const std::string& requestHead,HttpRequest& result);
