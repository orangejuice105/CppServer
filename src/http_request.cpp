#include "http_request.h"
#include "logger.h"

bool parseRequestLine(const std::string& line,HttpRequest& result){
    //请求行的基本结构： 方法 + 一个空格 + 请求目标 + 一个空格 + HTTP版本

    //先找第一个空格
    std::size_t firstSpace = line.find(' ');

    if(firstSpace == std::string::npos){
        LOGGER_ERROR("无法分出方法和请求目标");
        return false;
    }

    if(firstSpace == 0){
        LOGGER_ERROR("方法为空");
        return false;
    }

    //确认第一个空格有效后找第二个空格
    std::size_t secondSpace = line.find(' ', firstSpace + 1);

    if(secondSpace == std::string::npos){
        LOGGER_ERROR("缺少后面的分隔部分");
        return false;
    }

    if(secondSpace == firstSpace + 1){
        LOGGER_ERROR("两个空格紧挨着，请求目标为空");
        return false;
    }

    if(secondSpace + 1 >= line.size()){
        LOGGER_ERROR("第二个空格之后没有版本");
        return false;
    }

    if(line.find(' ',secondSpace + 1) != std::string::npos){
        LOGGER_ERROR("请求行不符合三段格式");
        return false;
    }

    //检查通过后填写result
    result.method = line.substr(0,firstSpace);
    result.target = line.substr(firstSpace + 1,secondSpace - firstSpace - 1);
    result.version = line.substr(secondSpace + 1);

    return true;
}
