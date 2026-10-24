#pragma once
#include <string>
#include <vector>
#include <cstddef>

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
    std::string authority; //请求目标中的主机与端口，如有
    std::string pathAndQuery; //路径与查询部分
};




//声明解析函数
bool parseRequestLine(const std::string& line,HttpRequest& result); //line: 要解析的请求行，不包含结尾的\r\n

//声明解析一行请求头函数
bool parseHeaderLine(const std::string& line,HttpHeader& result);

//声明解析整块请求头的函数
bool parseRequestHeaders(const std::string& requestHead,HttpRequest& result);

//检查host数量和字段值
bool validateHostHeader(const HttpRequest& request);

//检查host字段值是否合法
bool validateHostValue(const std::string& value);

//判断字符是不是十六进制字符
bool isHexDigit(char ch);

//检查已经取出的主机部分，例如localhost
bool validateRegName(const std::string& host);

//检查connect请求目标
bool validateConnectTarget(const std::string& target);

//检查路径和查询部分允许的字符
bool validatePathAndQueryChars(const std::string& text);

//拆URL形式的请求目标为主机与端口、路径与查询两部分
bool parseHttpAbsoluteTarget(const std::string&target,std::string& authority,std::string& pathAndQuery);

//检查Content-Length的格式和是否重复
bool validateContentLengthHeader(const HttpRequest& request);

//检查合法Content-Length的长度是否大于0
bool hasNonZeroContentLength(const HttpRequest& request);

//查找是否有名为fieldName的请求头
bool hasRequestHeader(const HttpRequest& request,const std::string& fieldName);

//检查Transfer-Encoding的单个传输编码是否合法并转成小写
bool parseTransferCodingName(const std::string& text,std::string& result);

//寻找引号外面的逗号(处理编码带参数值的情况)
bool findTransferCodingComma(const std::string& value,std::size_t start,std::size_t& comma);

//处理单个编码里的带双引号参数值；成功时pos移到结束引号后，失败时不变
bool parseTransferQuotedString(const std::string& text,std::size_t& pos);

//判断一个字符能不能出现在token中
bool isHttpTokenChar(char ch);

//组合现有函数完成解析单个传输编码及其参数，成功时输出小写编码名称
bool parseTransferCodingItem(const std::string& text,std::string& result);

//解析Transfer-Encoding列表，检查每项名称及参数，输出小写编码名称列表
bool parseTransferCodingList(const std::string&value,std::vector<std::string>& result);

//从整个请求找出所有Transfer-Encoding字段，按顺序合并再解析成编码列表
bool parseTransferEncodingHeaders(const HttpRequest& request,std::vector<std::string>& result);

//检查已解析的请求编码列表，chunked只能在末尾出现且只能出现一次
bool validateRequestTransferCodingOrder(const std::vector<std::string>& codings);

//检查已接收数据中的请求目标是否超限；返回true时，method保存方法名
bool isRequestTargetTooLong(const std::string& request,std::size_t maxTargetBytes,std::string& method);