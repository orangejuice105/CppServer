#pragma once
#include <string>

//生成HTTP日期字段值，使用UTC；失败返回空字符串
//HTTP 日期使用 UTC 时间，并固定使用英文星期、英文月份和 GMT 标记
//返回值示例：Sun, 06 Nov 1994 08:49:37 GMT（不包含字段名 Date:）
std::string makeHttpDate();

//生成响应字符串
std::string buildHttpResponse(
    const std::string& responseVersion,
    int statusCode,
    const std::string& reasonPhrase,
    const std::string& body,
    bool isHeadRequest);
