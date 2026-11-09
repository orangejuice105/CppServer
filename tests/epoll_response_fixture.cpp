// Test-only: enlarge a marked /hello response, keep production IO, clocks and event loop.
#include "tcp_connection.h"
#include "http_response.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
extern "C" ReceiveResult __real__Z14receiveRequestR13TcpConnection(TcpConnection&);
extern "C" int __real_accept(int,sockaddr*,socklen_t*);
extern "C" ssize_t __real_send(int,const void*,size_t,int);
void trace(const char* kind,int fd,long long value){
    const int saved=errno;
    const char* path=std::getenv("CPP_SEND_FIXTURE_TRACE");
    if(path){timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);char buffer[192];const int size=std::snprintf(buffer,sizeof(buffer),"%s %d %lld %lld\n",kind,fd,static_cast<long long>(now.tv_sec)*1000000000LL+now.tv_nsec,value);int out=open(path,O_WRONLY|O_CREAT|O_APPEND,0600);if(out>=0){(void)write(out,buffer,static_cast<size_t>(size));close(out);}}
    errno=saved;
}
extern "C" ReceiveResult __wrap__Z14receiveRequestR13TcpConnection(TcpConnection& connection){
    const auto result=__real__Z14receiveRequestR13TcpConnection(connection);
    if(result==ReceiveResult::ResponseReady&&connection.request.find("X-Test-Large: 1\r\n")!=std::string::npos&&connection.response.find("HTTP/1.1 200 OK\r\n")==0){
        connection.response=buildHttpResponse("HTTP/1.1",200,"OK",std::string(8*1024*1024,'L'),false);
        connection.sentBytes=0;
        trace("FIXTURE",connection.fd,static_cast<long long>(connection.response.size()));
    }
    return result;
}
extern "C" int __wrap_accept(int fd,sockaddr* address,socklen_t* length){
    const int client=__real_accept(fd,address,length);const int saved=errno;
    if(client>=0){int size=8192;if(setsockopt(client,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))!=0)trace("BUFFER_ERROR",client,errno);}
    errno=saved;return client;
}
extern "C" ssize_t __wrap_send(int fd,const void* data,size_t length,int flags){
    const auto result=__real_send(fd,data,length,flags);const int saved=errno;
    if(result>0)trace("SEND",fd,result);
    else if(result<0&&(saved==EAGAIN||saved==EWOULDBLOCK))trace("EAGAIN",fd,saved);
    errno=saved;return result;
}
