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

    //检查HTTP版本字段格式
    /*字符： H T T P / 1 . 1
      下标： 0 1 2 3 4 5 6 7 */
    std::string version = line.substr(secondSpace + 1);
    if(version.size() != 8){
        LOGGER_ERROR("版本字段长度不为8");
        return false;
    }
    if(version.substr(0,5) != "HTTP/"){
        LOGGER_ERROR("前5个字符不为HTTP/");
        return false;
    }
    if(version[6] != '.'){
        LOGGER_ERROR("下标6不为.");
        return false;
    }
    bool majorIsDigit = version[5] >= '0' && version[5] <= '9';
    bool minorIsDigit = version[7] >= '0' && version[7] <= '9';
    if(!majorIsDigit){
        LOGGER_ERROR("下标5不是数字字符");
        return false;
    }
    if(!minorIsDigit){
        LOGGER_ERROR("下标7不是数字字符");
        return false;
    }

    //检查通过后填写result
    result.method = line.substr(0,firstSpace);
    result.target = line.substr(firstSpace + 1,secondSpace - firstSpace - 1);
    result.version = version;

    return true;
}

bool parseHeaderLine(const std::string& line,HttpHeader& result){
    //找第一个冒号
    std::size_t colon = line.find(':');

    if(colon == std::string::npos){
        LOGGER_ERROR("未找到冒号");
        return false;
    }

    if(colon == 0){
        LOGGER_ERROR("冒号前面没有字段名");
        return false;
    }

    //找到并且无误后提取字段名和字段值
    std::string name = line.substr(0,colon);
    std::string value = line.substr(colon + 1);

    //检查字段名是否合法
    //字段名允许ASCII字母、数字和以下符号（!#$%&'*+-.^_`|~）
    const std::string tokenSymbols = "!#$%&'*+-.^_`|~";

    for(char ch : name){
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            continue;
        }
        if(ch >= '0' && ch <= '9'){
            continue;
        }

        std::size_t check = tokenSymbols.find(ch);
        if(check != std::string::npos){
            continue;
        }
        return false;
    }

    //检查通过后将字段名统一转换成小写
    for(char& ch : name){
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    //去掉字段值两端的空格和制表符
    //先找第一个不是空格或制表符的位置
    std::size_t first = value.find_first_not_of(" \t"); //find_first_not_of: 从前往后找第一个不属于这些字符的位置

    //没找到说明值为空，把它设置为空字符串
    if(first == std::string::npos){
        value = "";
    }
    else {
        //再找最后一个不是空白的位置
        std::size_t last = value.find_last_not_of(" \t");

        value = value.substr(first,last - first + 1);
    }

    //清理字段值中的非法控制字符（换行、空字符等，制表符可以保留）
    //使用unsigned char把字节按0~255的范围判断，避免普通char在某些平台出现负值
    for(unsigned char ch : value){
        if((ch < 32 && ch != '\t') || ch == 127){
            return false;
        }
    }

    //  全部检查通过则填写结果
    result.name = name;
    result.value = value;

    return true;
}

bool parseRequestHeaders(const std::string& requestHead, HttpRequest& result){
    //先准备临时列表
    std::vector<HttpHeader> parsedHeaders;

    //接着找到请求行末尾
    std::size_t firstLineEnd = requestHead.find("\r\n");

    if(firstLineEnd == std::string::npos){
        LOGGER_ERROR("未找到请求行末尾");
        return false;
    }

    //找到之后准备当前位置
    std::size_t pos = firstLineEnd + 2; //+2跳过\r\n,找到第一个请求头字段

    while(pos < requestHead.size()){
        //寻找当前行的结束位置
        std::size_t end = requestHead.find("\r\n",pos);

        if(end == std::string::npos){
            LOGGER_ERROR("该行没有正确结束");
            return false;
        }
        if(end == pos){
            if(end + 2 != requestHead.size()){
                LOGGER_ERROR("空行之后还有多余内容，不符合本函数的输入约定");
                return false;
            }
            //相等则说明已经到达整块头部末尾，保存结果并返回
            else {
                result.headers = parsedHeaders;
                return true;
            }
        }

        std::string line = requestHead.substr(pos,end - pos);
        HttpHeader header;

        if(parseHeaderLine(line,header)){
            parsedHeaders.push_back(header);
        }
        else {
            LOGGER_ERROR("整块请求头解析失败");
            return false;
        }

        //移动到下一行
        pos = end + 2;
    }
    return false;
}
