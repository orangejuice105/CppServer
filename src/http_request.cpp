#include "http_request.h"
#include "logger.h"
#include <arpa/inet.h>
#include <netinet/in.h>

bool parseRequestLine(const std::string& line,HttpRequest& result){
    //请求行的基本结构： 方法 + 一个空格 + 请求目标 + 一个空格 + HTTP版本
                        //GET /hello?name=Tom HTTP/1.1


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

    //让服务器区分是方法名称写错了还是名称合法但服务器不认识
    //明确方法名称允许哪些字符(除下列还有数字和大小写字母)
    const std::string tokenSymbols = "!#$%&'*+-.^_`|~";

    //提取方法
    std::string method = line.substr(0,firstSpace);

    for(char ch : method){
        if((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')){
            continue;
        }
        if(tokenSymbols.find(ch) != std::string::npos){
            continue;
        }

        LOGGER_WARN("HTTP方法名称包含非法字符");
        return false;
    }

    //检查请求目标是否合法
    //提取请求目标
    std::string target = line.substr(firstSpace + 1,secondSpace - firstSpace - 1);

    for(std::size_t i = 0; i < target.size();i++){
        unsigned char ch = static_cast<unsigned char>(target[i]); //转换为按0~255表示的字节

        if(ch <= 32 || ch >= 127){
            LOGGER_WARN("请求目标包含非法字节");
            return false;
        }

        if(ch == '#'){
            LOGGER_WARN("请求目标包含#");
            return false;
        }

        if(ch == '%'){
            if(target.size() - i < 3){
                LOGGER_WARN("请求目标百分号编码不完整");
                return false;
            }
            if(!isHexDigit(target[i + 1]) || !isHexDigit(target[i + 2])){
                LOGGER_WARN("%后面两字符存在非十六进制字符");
                return false;
            }

            i += 2;
            continue;
        }
    }

    //检查请求目标的形式是否合法
    /*HTTP 请求目标主要有四种形式：
    形式	        请求行例子	                                    目标表达什么
    普通路径形式	GET /hello?name=Tom HTTP/1.1	                路径和查询参数
    绝对 URI 形式	GET http://127.0.0.1:8080/hello HTTP/1.1	   包含协议、主机等信息的完整 URI
    主机与端口形式	CONNECT example.com:443 HTTP/1.1	            CONNECT 要连接的主机和端口
    星号形式	    OPTIONS * HTTP/1.1	                           整个服务器*/

    //四个分支
    std::string authority;
    std::string pathAndQuery;

    //处理CONNECT的主机与端口
    if(method == "CONNECT"){
        if(!validateConnectTarget(target)){
            return false;
        }
        authority = target;
    }

    //处理星号分支
    else if(target == "*"){
        if(method != "OPTIONS"){
            LOGGER_ERROR("星号形式请求目标只允许用于OPTIONS");
            return false;
        }
    }

    //处理普通路径分支
    else if(target.front() == '/'){
        if(!validatePathAndQueryChars(target)){
            return false;
        }
        pathAndQuery = target;
    }

    //解析完整URL
    else{
        if (!parseHttpAbsoluteTarget(target, authority, pathAndQuery)) {
            return false;
        }
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
    result.method = method;
    result.target = target;
    result.version = version;
    result.authority = authority;
    result.pathAndQuery = pathAndQuery;

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

bool validateHostHeader(const HttpRequest& request){
    //计数器
    std::size_t hostCount = 0;

    for(const HttpHeader& header : request.headers){
        if(header.name == "host"){
            hostCount++;
            if (!validateHostValue(header.value)) {
                return false;
            }
        }
    }

    //host最多一个
    if(hostCount > 1){
        LOGGER_ERROR("host字段重复");
        return false;
    }

    //只有HTTP/1.0才支持不带host
    if(hostCount == 0 && request.version != "HTTP/1.0"){
        LOGGER_ERROR("缺少host字段");
        return false;
    }

    return true;
}

bool validateHostValue(const std::string& value){
    //寻找非法字符
    std::size_t invalidPos = value.find_first_of(" \t/?#@\\"); //find_first_of：找第一个属于指定字符集合的字符

    if(invalidPos != std::string::npos){
        LOGGER_WARN("Host字段值包含非法字符");
        return false;
    }

    //检查端口是否只包含数字
    //处理空字符串
    if(value.empty()){
        return true;
    }

    std::string port;

    //处理字符串，根据字符串第一个字符选择处理分支
    if(value.front() == '['){ //方括号地址分支
        //先找到右方括号
        std::size_t closeBracket = value.find(']');

        if(closeBracket == std::string::npos){
            LOGGER_ERROR("括号没有闭合");
            return false;
        }

        if(closeBracket == 1){
            LOGGER_ERROR("输入以 [] 开头，括号里面没有地址");
            return false;
        }

        //方括号内地址内容检查
        std::string ipLiteral = value.substr(1,closeBracket - 1);
            //准备存放转换结果的对象
        in6_addr address{};
                                        /*
                                        AF_INET6	按 IPv6 地址格式解析
                                        ipLiteral.c_str()	提供要解析的文本,c_str() 返回指向字符串内容的 const char*
                                        &address	提供存放转换结果的位置
                                        */
        int addressResult = inet_pton(AF_INET6,ipLiteral.c_str(),&address);
        if(addressResult != 1){
            LOGGER_ERROR("IPv6 地址校验未成功,addressResult = "
             + std::to_string(addressResult));
            return false;
        }
        //计算右方括号后面的位置
        std::size_t afterHost = closeBracket + 1;

        //如果等于value.size()说明没有端口
        if(afterHost < value.size()){
            if(value[afterHost] != ':'){
                LOGGER_ERROR("当前位置不为:");
                return false;
            }
            else{
                port = value.substr(afterHost + 1);
            }
        }
    }
    //普通分支
    else{
        std::size_t colon = value.find(':');

        //取出主机部分并调用
        std::string host = value.substr(0,colon);
        if(!validateRegName(host)){
            return false;
        }

        if(colon != std::string::npos){
            port = value.substr(colon + 1);
        }

        //如果等于std::string::npos说明没有端口部分，保持port为空
    }

    for(char ch : port){
        if(ch < '0' || ch > '9'){
            LOGGER_ERROR("Host端口包含非数字字符");
            return false;
        }
    }
    return true;
}

bool isHexDigit(char ch){
    if((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')){
        return true;
    }
    return false;
}

bool validateRegName(const std::string& host){
    //准备允许的符号合集
    const std::string allowedSymbols = "-._~!$&'()*+,;=";

    for(std::size_t i = 0;i < host.size();i++){
        char ch = host[i];
        if((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')){
            continue;
        }
        if(allowedSymbols.find(ch) != std::string::npos){
            continue;
        }

        if(ch == '%'){
            if(i + 3 > host.size()){
                LOGGER_ERROR("剩余长度不足");
                return false;
            }

            if(!isHexDigit(host[i + 1]) || !isHexDigit(host[i + 2])){
                LOGGER_ERROR("字符不合法");
                return false;
            }

            i += 2;
            continue;
        }
        //如果上面三个检查分支都没进去说明主机名包含非法字符
        LOGGER_WARN("主机名包含非法字符");
        return false;
    }

    return true;
}

bool validateConnectTarget(const std::string& target){
    //找到分隔端口的冒号
    std::size_t portColon = target.rfind(':');

    if(portColon == std::string::npos){
        LOGGER_WARN("没有端口分隔符");
        return false;
    }

    if(portColon == 0){
        LOGGER_WARN("冒号前没有主机");
        return false;
    }

    if(portColon + 1 == target.size()){
        LOGGER_WARN("冒号后没有端口");
        return false;
    }

    std::string host = target.substr(0,portColon);
    std::string portText = target.substr(portColon + 1);

    //处理IPv6的情况
    if(host.front() == '[' && host.back() != ']'){
        LOGGER_WARN("CONNECT 目标的方括号主机不完整");
        return false;
    }
    if(!validateHostValue(target)){
        return false;
    }

    //检查端口数值范围(1~65535)
    int portNumber = 0;
    for(char ch : portText){
        portNumber = portNumber * 10 + (ch - '0');
        if(portNumber > 65535){
            LOGGER_WARN("端口数值超出范围");
            return false;
        }
    }
    if(portNumber == 0){
        LOGGER_WARN("端口数值为0");
        return false;
    }

    return true;
}

bool validatePathAndQueryChars(const std::string& text){
 //   http://127.0.0.1:8080/hello?name=Tom
 //   │      │             │     │
 //  协议   主机和端口    路径  查询参数

     //允许的符号合集
    const std::string originSymbols = "-._~!$&'()*+,;=:@/?%";

    for(char ch : text){
        if((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')){
            continue;
        }

        if(originSymbols.find(ch) != std::string::npos){
            continue;
        }

        //如果前两项都不满足则返回警告
        LOGGER_WARN("路径或查询部分包含非法字符");
        return false;
    }
    return true;
}

bool parseHttpAbsoluteTarget(const std::string& target,std::string& authority,std::string& pathAndQuery){
    //准备前缀
    const std::string httpPrefix = "http://";
    std::string prefix = target.substr(0,httpPrefix.size());

    for(char& ch : prefix){
        if(ch >= 'A' && ch <= 'Z'){
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    if(prefix != httpPrefix){
        LOGGER_WARN("本函数只处理 http:// 形式的请求目标");
        return false;
    }

    //确定主机与端口的起始和结束位置
    std::size_t authorityStart = httpPrefix.size();
    std::size_t authorityEnd = target.find_first_of("/?",authorityStart);

    //如果没找到分隔符，说明主机与端口一直延续到字符串末尾，如：http://localhost:8080
    if(authorityEnd == std::string::npos){
        authorityEnd = target.size();
    }

    //截出主机与端口
    std::string authorityText = target.substr(authorityStart,authorityEnd - authorityStart);

    if(authorityText.empty()){
        LOGGER_WARN("HTTP URL的主机不能为空");
        return false;
    }

    if(authorityText.front() == ':'){
        LOGGER_WARN(" : 前没有主机");
        return false;
    }

    //检查主机字符、IPv6 方括号地址、端口是否包含非数字字符等
    if (!validateHostValue(authorityText)) {
        return false;
    }

    //取出路径与查询部分
    //准备一个局部字符串
    std::string remaining;

    //主机部分结束后没有剩余内容则省略路径按根路径 / 处理
    if(authorityEnd == target.size()){
        remaining = '/';
    }

    //没有路径但有查询参数时，补上路径
    else if(target[authorityEnd] == '?'){
        remaining = '/' + target.substr(authorityEnd);
    }

    else{
        remaining = target.substr(authorityEnd);
    }

    //再检查路径和查询部分是否合法
    if (!validatePathAndQueryChars(remaining)) {
        return false;
    }

    authority = authorityText;
    pathAndQuery = remaining;
    return true;
}

bool validateContentLengthHeader(const HttpRequest& request){
    //计数器
    std::size_t contentLengthCount = 0;

    //判断是否重复或者字段值是否合法
    for(const HttpHeader& header : request.headers){
        if (header.name != "content-length") {
            continue;
        }
        contentLengthCount++;

        if(contentLengthCount > 1){
            LOGGER_ERROR("Content-Length 字段重复");
            return false;
        }

        if(header.value.empty()){
            LOGGER_ERROR("Content-Length 字段值为空");
            return false;
        }

        for(char ch : header.value){
            if(ch < '0' || ch > '9'){
                LOGGER_ERROR("Content-Length 包含非数字字符");
                return false;
            }
        }
    }

    return true;
}

bool hasNonZeroContentLength(const HttpRequest& request){
    for (const HttpHeader& header : request.headers){
        if (header.name != "content-length") {
            continue;
        }

        //找第一个非0的字符
        std::size_t nonZeroPos = header.value.find_first_not_of('0');

        if(nonZeroPos != std::string::npos){
            return true;
        }
    }
    //没找到非0的Content-Length
    return false;
}

bool hasRequestHeader(const HttpRequest& request,const std::string& fieldName){
    for (const HttpHeader& header : request.headers){
        if(header.name == fieldName){
            return true;
        }
    }
    return false;
}

bool isHttpTokenChar(char ch){
    const std::string tokenSymbols = "!#$%&'*+-.^_`|~";

    if((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')){
        return true;
    }
    if(tokenSymbols.find(ch) != std::string::npos){
        return true;
    }

    return false;
}

bool parseTransferCodingName(const std::string& text,std::string& result){
    if(text.empty()){
        LOGGER_WARN("传输编码名称为空");
        return false;
    }

    //准备允许的符号
    const std::string tokenSymbols = "!#$%&'*+-.^_`|~";

    for(char ch : text){
        if((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')){
            continue;
        }
        if(tokenSymbols.find(ch) != std::string::npos){
            continue;
        }

        LOGGER_WARN("传输编码名称包含非法字符");
        return false;
    }

    //将合法的传输编码统一转成小写
    std::string normalized = text;

    for(char& ch : normalized){
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    result = normalized;
    return true;
}

bool findTransferCodingComma(const std::string& value,std::size_t start,std::size_t& comma){
    // Transfer-Encoding: x-demo; note="a,b", chunked
    // x-demo，它带有参数 note="a,b",里面的 , 不是分隔逗号

    std::size_t found = std::string::npos; //找到的分隔逗号的下标，默认没有找到
    bool inQuotes = false; //目前是否处于双引号里面
    bool escaped = false; //前一个字符是否为引号里面的反斜杠

    //从start开始逐个检查字符
    for(std::size_t i = start;i < value.size();i++){
        char ch = value[i];

        //如果前一个字符为'\',当前字符当做普通字符处理
        if(escaped){
            escaped = false;
            continue;
        }

        //在引号里遇到了反斜杠
        if(inQuotes && ch == '\\'){
            escaped = true;
            continue;
        }

        //遇到未被反斜杠转义的双引号切换状态
        if(ch == '"'){
            inQuotes = !inQuotes;
        }

        //在引号外面遇到了逗号
        if(!inQuotes && ch == ','){
            found = i;
            break;
        }
    }

    //循环结束后检查是否闭合
    if(inQuotes){
        LOGGER_WARN("引号未闭合");
        return false;
    }

    comma = found;
    return true;
}

bool parseTransferQuotedString(const std::string& text,std::size_t& pos){
    //检查当前位置
    if(pos >= text.size() || text[pos] != '"'){
        LOGGER_WARN("pos位置错误");
        return false;
    }

    bool escaped = false;
    for(std::size_t i = pos + 1;i < text.size();i++){
        unsigned char ch = static_cast<unsigned char>(text[i]);

        //处理非法控制字符
        if((ch < 32 && ch != '\t') || ch == 127){
            LOGGER_WARN("存在非法控制字符");
            return false;
        }

        //处理转义
        if(escaped){
            escaped = false;
            continue;
        }

        if(ch == '\\'){
            escaped = true;
            continue;
        }

        //遇到未被转义的双引号，解析成功
        if(ch == '"'){
            pos = i + 1;
            return true;
        }
    }

    //循环结束没有返回说明引号未闭合
    LOGGER_WARN("引号没闭合");
    return false;
}

bool parseTransferCodingItem(const std::string& text,std::string& result){
    //准备当前位置
    std::size_t pos = 0;

    //跳过开头的空格和制表符
    while(pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')){
        pos++;
    }

    std::size_t nameStart = pos;
    while(pos < text.size() && isHttpTokenChar(text[pos])){
        pos++;
    }
    //截取编码名称
    std::string name = text.substr(nameStart,pos - nameStart);

    std::string coding;
    if(!parseTransferCodingName(name,coding)){
        return false;
    }

    //处理编码的参数
    // Transfer-Encoding: x-demo; note="a,b", chunked

    //不允许携带参数的传输编码
    bool forbidsParameters =
    coding == "chunked" || coding == "gzip" || coding == "x-gzip" ||
    coding == "compress" || coding == "x-compress" || coding == "deflate";

    while(true){
        //先跳过空白,再找到分号
        while(pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')){
            pos++;
        }
        //如果到末尾说明没有参数了
        if(pos == text.size()){
            break;
        }

        if(text[pos] != ';'){
            LOGGER_WARN("传输编码参数前缺少分号");
            return false;
        }

        if(forbidsParameters){
            LOGGER_WARN("该传输编码不允许携带参数");
            return false;
        }

        //跳过分号
        pos++;
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }

        //检查参数名称是否合法
        std::size_t parameterStart = pos;
        while (pos < text.size() && isHttpTokenChar(text[pos])) {
            ++pos;
        }

        if (pos == parameterStart) {
            LOGGER_WARN("传输编码参数名称为空或起始字符非法");
            return false;
        }

        //传输编码参数的语法允许接收端处理等号两侧的空白
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
            ++pos;
        }

        if(pos >= text.size() || text[pos] != '='){
            LOGGER_WARN("传输编码参数缺少等号");
            return false;
        }
        //越过等号
        pos++;

        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
            ++pos;
        }

        if (pos >= text.size()) {
            LOGGER_WARN("传输编码参数缺少值");
            return false;
        }

        //分别处理参数值带双引号和不带双引号
        if(text[pos] == '"'){
            if(!parseTransferQuotedString(text,pos)){
                return false;
            }

        }
        else{
            std::size_t valueStart = pos;

            while(pos < text.size() && isHttpTokenChar(text[pos])){
                pos++;
            }

            if(pos == valueStart){
                LOGGER_WARN("传输编码参数值为空或起始字符非法");
                return false;
            }
        }
    }
    result = coding;
    return true;
}

bool parseTransferCodingList(const std::string&value,std::vector<std::string>& result){
    std::vector<std::string> parsed; //暂存已经解析成功的名称
    std::size_t pos = 0; //这一轮字符串开始寻找的位置

    while(true){
        //寻找分隔的 ,
        std::size_t comma = std::string::npos;
        if (!findTransferCodingComma(value, pos, comma)) {
            return false;
        }
        std::size_t end = value.size(); //默认结束位置，如果找到逗号则改为逗号的位置

        if(comma != std::string::npos){
            end = comma;
        }

        //截取当前段
        std::string item = value.substr(pos,end - pos);

        //去掉当前段两端的空格和制表符
        std::size_t first = item.find_first_not_of(" \t");

        if(first != std::string::npos){
            std::size_t last = item.find_last_not_of(" \t");
            std::string itemText = item.substr(first,last - first + 1);

            //解析当前整项，检查名称和参数，得到小写编码名称
            std::string coding;
            if(!parseTransferCodingItem(itemText,coding)){
                return false;
            }
            parsed.push_back(coding);
        }


        //循环末尾决定结束还是移动到下一段
        //没找到逗号说明已经处理了最后一段
        if(comma == std::string::npos){
            break;
        }
        else{
            pos = comma + 1;
        }
    }

    if(parsed.empty()){
        LOGGER_WARN("传输列表没有有效名称");
        return false;
    }

    result = parsed;
    return true;
}

bool parseTransferEncodingHeaders(const HttpRequest& request,std::vector<std::string>& result){
    /*  Transfer-Encoding: gzip             得到
        X-Test: abc                         ---->  ["gzip", "chunked"]
        Transfer-Encoding: chunked*/

        std::string combined; //保存合并后的字段值
        bool found = false; //记录是否已经遇到过Transfer-Encoding字段

        //遍历所有请求头，只处理目标字段
        for(const HttpHeader& header : request.headers){
            if (header.name != "transfer-encoding") {
                continue;
            }

            //如果已经遇到过目标字段，先补分隔符
            if(found){
                combined += ',';
            }

            combined += header.value;
            found = true;
        }

        //处理没有字段的情况
        if(!found){
            result.clear(); //清空输出列表中的所有元素，让调用者明确得到没有传输编码
            return true;
        }

        //有字段时交给列表函数解析
        return parseTransferCodingList(combined, result);
}

bool validateRequestTransferCodingOrder(const std::vector<std::string>& codings){
    //先检查空列表
    if(codings.empty()){
        LOGGER_WARN("请求传输编码列表为空");
        return false;
    }

    std::size_t chunkedCount = 0;
    for(const std::string& coding : codings){
        if(coding == "chunked"){
            chunkedCount++;
        }

        if(chunkedCount > 1){
            LOGGER_WARN("请求中的chunked重复出现");
            return false;
        }
    }

    if(chunkedCount == 0){
        LOGGER_WARN("请求传输编码缺少chunked");
        return false;
    }

    if(codings.back() != "chunked"){
        LOGGER_WARN("请求中的chunked必须位于编码列表末尾");
        return false;
    }

    return true;
}

bool isRequestTargetTooLong(const std::string& request,std::size_t maxTargetBytes,std::string& method){
    //先找第一个空格（它将方法和目标分开）
    std::size_t firstSpace = request.find(' ');
    if(firstSpace == std::string::npos || firstSpace == 0){
        return false;
    }

    //确认空格前的内容是否可以作为方法名
    for(std::size_t i = 0;i < firstSpace;i++){
        if(!isHttpTokenChar(request[i])){
            return false;
        }
    }

    //确定请求目标开始位置
    std::size_t targetStart = firstSpace + 1;

    //寻找当前能够确定的目标结束位置
    std::size_t targetEnd = request.find_first_of(" \r\n",targetStart);

    //没找到就算到数据末尾
    if(targetEnd == std::string::npos){
        targetEnd = request.size();
    }

    //判断长度，超限时输出方法名
    if(targetEnd - targetStart <= maxTargetBytes){
        return false;
    }

    method = request.substr(0,firstSpace);
    return true;
}