// Test-only direct production function checks; only EINTR is injected.
#include "tcp_connection.h"
#include "logger.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unordered_map>
#include <unistd.h>
#include <chrono>
#include <string>
#include <iostream>
#include <stdexcept>
#include <cerrno>
#include <algorithm>

using Clock=std::chrono::steady_clock;
void removeConnection(int,std::unordered_map<int,TcpConnection>&,int);
void handleConnectionEvent(int,std::unordered_map<int,TcpConnection>&,const epoll_event&);
extern "C" ssize_t __real_recv(int,void*,size_t,int);
bool interruptRecv=false;
int recvCalls=0;
extern "C" ssize_t __wrap_recv(int fd,void* data,size_t length,int flags){
    ++recvCalls;
    if(interruptRecv&&recvCalls==1){errno=EINTR;return -1;}
    return __real_recv(fd,data,length,flags);
}
extern "C" ssize_t __real_send(int,const void*,size_t,int);
int calls=0;
bool injectInterrupt=false;
bool retryPreserved=true;
TcpConnection* active=nullptr;
Clock::time_point expectedDeadline{};
extern "C" ssize_t __wrap_send(int fd,const void* data,size_t length,int flags){
    ++calls;
    if(injectInterrupt && calls==1){errno=EINTR;return -1;}
    if(injectInterrupt && active && active->sendIdleDeadline!=expectedDeadline) retryPreserved=false;
    return __real_send(fd,data,length,flags);
}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Pair{
    int fd[2]{-1,-1};
    Pair(){require(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,fd)==0,"socketpair failed (possible sandbox restriction)");}
    ~Pair(){for(int item:fd)if(item>=0)close(item);}
};
TcpConnection connectionFor(int fd,const std::string& response){
    TcpConnection connection;
    connection.fd=fd;
    connection.phase=ConnectionPhase::Sending;
    connection.response=response;
    connection.sendIdleDeadline=Clock::now()+std::chrono::seconds(5);
    connection.receiveIdleDeadline=Clock::now()+std::chrono::seconds(30);
    connection.requestDeadline=Clock::now()+std::chrono::seconds(60);
    return connection;
}
void noPeerBytes(int fd){char byte;errno=0;require(recv(fd,&byte,1,0)==-1&&(errno==EAGAIN||errno==EWOULDBLOCK),"unexpected peer bytes");}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    LoggerConfig config;config.filePath=argv[1];
    if(!Logger::getInstance().init(config))return 3;
    int passed=0;
    try{
        {
            Pair sockets;
            auto connection=connectionFor(sockets.fd[0],"response");
            connection.sendIdleDeadline=Clock::now()-std::chrono::seconds(1);
            const auto previous=connection.sendIdleDeadline;calls=0;
            require(sendResponse(connection)==SendResult::CloseConnection,"expired send did not request close");
            require(calls==0&&connection.sentBytes==0&&connection.sendIdleDeadline==previous,"expired send performed IO or changed state");
            noPeerBytes(sockets.fd[1]);++passed;std::cout<<"PASS expired-before-send\n";
        }
        {
            Pair sockets;
            std::string data("abc\0def",7);
            auto connection=connectionFor(sockets.fd[0],data);
            require(sendResponse(connection)==SendResult::SendComplete,"small response did not complete (possible sandbox send restriction)");
            char buffer[32];const auto size=recv(sockets.fd[1],buffer,sizeof(buffer),0);
            require(size==static_cast<ssize_t>(data.size())&&std::string(buffer,static_cast<size_t>(size))==data,"small response bytes mismatch");
            require(connection.sentBytes==data.size(),"small response progress mismatch");
            ++passed;std::cout<<"PASS complete-response-bytes\n";
        }
        {
            Pair sockets;int size=4096;
            require(setsockopt(sockets.fd[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0,"set send buffer failed");
            std::string filler(1024,'x');
            while(true){
                const auto sent=__real_send(sockets.fd[0],filler.data(),filler.size(),MSG_NOSIGNAL);
                if(sent>0)continue;
                require(sent==-1&&(errno==EAGAIN||errno==EWOULDBLOCK),"buffer fill failed");break;
            }
            auto connection=connectionFor(sockets.fd[0],"pending response");
            const auto previous=connection.sendIdleDeadline;
            require(sendResponse(connection)==SendResult::NeedWrite,"full buffer did not need write");
            require(connection.sentBytes==0&&connection.sendIdleDeadline==previous,"EAGAIN changed progress or deadline");
            ++passed;std::cout<<"PASS real-EAGAIN-preserves-deadline\n";
        }
        {
            Pair sockets;int size=4096;
            require(setsockopt(sockets.fd[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0,"set send buffer failed");
            auto connection=connectionFor(sockets.fd[0],std::string(1024*1024,'p'));
            const auto before=Clock::now();
            require(sendResponse(connection)==SendResult::NeedWrite,"large response did not leave a remainder");
            const auto after=Clock::now();
            require(connection.sentBytes>0&&connection.sentBytes<connection.response.size(),"real partial send did not occur");
            require(connection.sendIdleDeadline>=before+std::chrono::seconds(30)&&connection.sendIdleDeadline<=after+std::chrono::seconds(30),"successful partial send deadline was not renewed by 30 seconds");
            std::string received;char buffer[8192];
            while(true){const auto count=recv(sockets.fd[1],buffer,sizeof(buffer),0);if(count>0){received.append(buffer,static_cast<size_t>(count));continue;}require(count==-1&&(errno==EAGAIN||errno==EWOULDBLOCK),"partial read failed");break;}
            require(received==connection.response.substr(0,connection.sentBytes),"partial send bytes/progress mismatch");
            ++passed;std::cout<<"PASS real-partial-send-renews-deadline\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"finished");
            connection.sentBytes=connection.response.size();connection.sendIdleDeadline=Clock::now()-std::chrono::seconds(1);calls=0;
            require(sendResponse(connection)==SendResult::SendComplete&&calls==0,"already completed response was treated as timeout");
            ++passed;std::cout<<"PASS completed-response-keeps-complete-result\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"retry");
            expectedDeadline=connection.sendIdleDeadline;active=&connection;calls=0;injectInterrupt=true;
            require(sendResponse(connection)==SendResult::SendComplete,"injected EINTR did not retry");
            require(calls==2&&retryPreserved,"EINTR changed deadline or retry count");
            injectInterrupt=false;active=nullptr;
            char buffer[32];const auto count=recv(sockets.fd[1],buffer,sizeof(buffer),0);
            require(count==5&&std::string(buffer,5)=="retry","retry bytes mismatch");
            ++passed;std::cout<<"PASS injected-EINTR-preserves-deadline\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"");
            connection.phase=ConnectionPhase::Receiving;
            const auto idle=connection.receiveIdleDeadline,total=connection.requestDeadline;
            require(receiveRequest(connection)==ReceiveResult::NeedMoreData,"empty nonblocking receive did not wait");
            require(connection.receiveIdleDeadline==idle&&connection.requestDeadline==total,"receive EAGAIN extended a deadline");
            ++passed;std::cout<<"PASS real-receive-EAGAIN-keeps-deadlines\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"");connection.phase=ConnectionPhase::Receiving;
            const auto total=connection.requestDeadline;
            const std::string first="GET /hello HTTP/1.1\r\nHost: localhost\r\n";
            require(__real_send(sockets.fd[1],first.data(),first.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(first.size()),"send fragment failed");
            const auto before=Clock::now();
            require(receiveRequest(connection)==ReceiveResult::NeedMoreData&&connection.request==first,"first fragment lost or completed early");
            require(connection.receiveIdleDeadline>=before+std::chrono::seconds(30)&&connection.requestDeadline==total,"successful receive deadline semantics wrong");
            require(__real_send(sockets.fd[1],"\r\n",2,MSG_NOSIGNAL)==2,"send final fragment failed");
            require(receiveRequest(connection)==ReceiveResult::ResponseReady&&connection.response.find("HTTP/1.1 200 OK\r\n")==0,"split request response failed");
            require(connection.response.substr(connection.response.find("\r\n\r\n")+4)=="Hello,HTTP!\n","split request body wrong");
            ++passed;std::cout<<"PASS real-split-receive-preserves-total-deadline\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"");connection.phase=ConnectionPhase::Receiving;
            const std::string raw="GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
            require(__real_send(sockets.fd[1],raw.data(),raw.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(raw.size()),"send request failed");
            recvCalls=0;interruptRecv=true;
            const auto result=receiveRequest(connection);interruptRecv=false;
            require(result==ReceiveResult::ResponseReady&&recvCalls==2,"receive EINTR did not retry");
            ++passed;std::cout<<"PASS injected-receive-EINTR-retries\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"");connection.phase=ConnectionPhase::Receiving;
            connection.request="HEAD /hello HTTP/1.0\r\nX-Wait: ";connection.requestDeadline=Clock::now()-std::chrono::seconds(1);
            require(checkReceiveTimeout(connection)==ReceiveResult::ResponseReady,"expired HEAD request did not prepare response");
            require(connection.response.find("HTTP/1.0 408 Request Timeout\r\n")==0,"timeout response version wrong");
            require(connection.response.substr(connection.response.find("\r\n\r\n")+4).empty(),"HEAD timeout included a body");
            require(connection.sentBytes==0,"timeout response progress not reset");
            ++passed;std::cout<<"PASS expired-HEAD-http10-response\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"");connection.phase=ConnectionPhase::Receiving;
            connection.receiveIdleDeadline=Clock::now()-std::chrono::seconds(1);
            const std::string raw="GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
            require(__real_send(sockets.fd[1],raw.data(),raw.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(raw.size()),"late request send failed");
            const auto deadline=connection.receiveIdleDeadline;
            require(receiveRequest(connection)==ReceiveResult::CloseConnection&&connection.request.empty()&&connection.receiveIdleDeadline==deadline,"late input revived expired connection");
            char bytes[128];require(__real_recv(sockets.fd[0],bytes,sizeof(bytes),0)==static_cast<ssize_t>(raw.size()),"expired receive consumed late bytes");
            ++passed;std::cout<<"PASS expired-receive-does-not-consume-or-renew\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"");connection.phase=ConnectionPhase::Receiving;
            connection.request="GET /hello HTTP/1.1\r\nHost: localhost\r\n";
            require(shutdown(sockets.fd[1],SHUT_WR)==0,"peer half-close failed");
            require(receiveRequest(connection)==ReceiveResult::ResponseReady&&connection.response.find("HTTP/1.1 400 Bad Request\r\n")==0,"partial request EOF did not prepare 400");
            ++passed;std::cout<<"PASS incomplete-header-EOF-prepares-400\n";
        }
        {
            Pair sockets;int epfd=epoll_create1(0);require(epfd>=0,"epoll creation failed");
            auto connection=connectionFor(sockets.fd[0],"");connection.phase=ConnectionPhase::Receiving;
            std::unordered_map<int,TcpConnection> connections;connections.emplace(connection.fd,connection);
            epoll_event add{};add.events=EPOLLIN;add.data.fd=connection.fd;
            require(epoll_ctl(epfd,EPOLL_CTL_ADD,connection.fd,&add)==0,"epoll add failed");
            const std::string raw="GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
            require(__real_send(sockets.fd[1],raw.data(),raw.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(raw.size()),"send request failed");
            epoll_event event{};require(epoll_wait(epfd,&event,1,1000)==1,"missing readable event");handleConnectionEvent(epfd,connections,event);
            require(connections.at(connection.fd).phase==ConnectionPhase::Sending,"receive did not enter sending");
            require(epoll_wait(epfd,&event,1,1000)==1&&(event.events&EPOLLOUT),"missing writable event");handleConnectionEvent(epfd,connections,event);
            require(connections.at(connection.fd).phase==ConnectionPhase::Draining,"send did not enter drain");
            std::string response;char bytes[1024];
            while(true){const auto count=__real_recv(sockets.fd[1],bytes,sizeof(bytes),0);if(count>0){response.append(bytes,static_cast<size_t>(count));continue;}require(count==0,"missing EOF after shutdown WR");break;}
            require(response.substr(response.find("\r\n\r\n")+4)=="Hello,HTTP!\n","event handler response mismatch");
            require(epoll_wait(epfd,&event,1,0)==0,"drain kept writable interest");
            require(__real_send(sockets.fd[1],"x",1,MSG_NOSIGNAL)==1,"tail send failed");
            require(epoll_wait(epfd,&event,1,1000)==1,"missing tail event");handleConnectionEvent(epfd,connections,event);
            require(connections.at(connection.fd).drainedBytes==1,"tail was not drained");
            require(shutdown(sockets.fd[1],SHUT_WR)==0,"peer shutdown failed");
            require(epoll_wait(epfd,&event,1,1000)==1,"missing EOF event");handleConnectionEvent(epfd,connections,event);
            require(connections.empty(),"EOF connection stayed in map");require(fcntl(connection.fd,F_GETFD)==-1&&errno==EBADF,"client descriptor leaked");sockets.fd[0]=-1;close(epfd);
            ++passed;std::cout<<"PASS real-epoll-receive-send-drain-release\n";
        }
        {
            Pair sockets;auto connection=connectionFor(sockets.fd[0],"");
            require(beginDrain(connection),"begin drain failed");const auto deadline=connection.drainDeadline,idle=connection.drainIdleDeadline;
            connection.drainedBytes=65535;require(beginDrain(connection)&&connection.drainedBytes==65535&&connection.drainDeadline==deadline&&connection.drainIdleDeadline==idle,"repeat drain reset state");
            require(__real_send(sockets.fd[1],"ab",2,MSG_NOSIGNAL)==2,"drain tail send failed");
            require(drainConnection(connection)==DrainResult::CloseConnection&&connection.drainedBytes==65536,"64 KiB equality not enforced");
            require(fcntl(connection.fd,F_GETFD)>=0,"drain helper closed caller-owned descriptor");
            ++passed;std::cout<<"PASS drain-byte-limit-and-repeat-preserve-state\n";
        }
        std::cout<<"RESULT "<<passed<<"/14; direct function and real epoll checks; long deadlines tested separately\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL after "<<passed<<" passes: "<<error.what()<<"; errno="<<errno<<'\n';return 1;}
}
