#include "http_response.h"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <locale>

std::string makeHttpDate(){
    std::time_t now = std::time(nullptr);

    //获取时间失败返回空字符串
    if(now == static_cast<std::time_t>(-1)){
        return "";
    }

    //把时间戳转换成UTC的年月日时分秒
    std::tm utcTime{}; //存放拆开的时间信息

    if(gmtime_r(&now,&utcTime) == nullptr){
        return "";
    }

    //准备字符串流并固定英文格式
    std::ostringstream output; //ostringstream: 专门用于向字符串输出内容
    //output.imbue(...)：让当前这个流使用这些规则
    output.imbue(std::locale::classic()); //std::locale::classic()：取得固定的经典格式规则

    //按 HTTP 日期格式写入字符串流
    //std::put_time() 根据格式字符串，把 std::tm 转成可读文字。它通过流的格式规则完成输出
    /*格式	含义	            示例
        %a	星期缩写	        Sun
        %d	两位日期，不足补零	 06
        %b	月份缩写	        Nov
        %Y	完整年份	        1994
        %H	24 小时制小时	    08
        %M	分钟	            49
        %S	秒	                37       */
    output << std::put_time(&utcTime,"%a, %d %b %Y %H:%M:%S GMT");

    //检查输出
    if(!output){
        return "";
    }

    return output.str(); //.str() 取得字符串流里已经生成的内容，作为日期函数的结果
}

std::string buildHttpResponse(
    const std::string& responseVersion,
    int statusCode,
    const std::string& reasonPhrase,
    const std::string& body,
    bool isHeadRequest)
{
    //构造状态行
    std::string response = responseVersion + " " + std::to_string(statusCode) + " " + reasonPhrase + "\r\n";

    //获取当前时间
    const std::string date = makeHttpDate();
    if(!date.empty()){
        response += "Date: " + date + "\r\n";
    }

    //遇到不允许的方法时，告诉客户端哪些方法允许
    if(statusCode == 405){
        response += "Allow: GET\r\n";
    }
    response += "Content-Type: text/plain; charset=utf-8\r\n";
    //处理HEAD的特殊需求; HEAD: 请求即使收到错误响应，服务器也不能发送正文
    if(!isHeadRequest)response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    if(!isHeadRequest)response += body;

    return response;
}
