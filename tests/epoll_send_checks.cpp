// Epoll send timeout probes: real nonblocking socketpair and original 30-second inactivity.
#include "tcp_connection.h"
#include "logger.h"
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <string>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <cerrno>
using Clock=std::chrono::steady_clock;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Pair{
    int fd[2]{-1,-1};
    Pair(){require(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,fd)==0,"socketpair failed");int size=4096;require(setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0,"setting send buffer failed");}
    ~Pair(){for(int item:fd)if(item>=0)close(item);}
};
std::string drain(int fd){std::string result;char buffer[8192];while(true){const auto count=recv(fd,buffer,sizeof(buffer),0);if(count>0){result.append(buffer,static_cast<size_t>(count));continue;}require(count==-1&&(errno==EAGAIN||errno==EWOULDBLOCK),"unexpected drain result");return result;}}
double seconds(Clock::duration duration){return std::chrono::duration<double>(duration).count();}
int main(int argc,char** argv){
    if(argc!=3)return 2;
    LoggerConfig config;config.filePath=argv[2];if(!Logger::getInstance().init(config))return 3;
    try{
        Pair pair;
        TcpConnection connection;connection.fd=pair.fd[0];connection.phase=ConnectionPhase::Sending;
        const std::string mode=argv[1];
        if(mode=="full-buffer-30s"){
            std::string filler(1024,'f');size_t filled=0;
            while(true){const auto result=send(pair.fd[0],filler.data(),filler.size(),MSG_NOSIGNAL);if(result>0){filled+=static_cast<size_t>(result);continue;}require(result==-1&&(errno==EAGAIN||errno==EWOULDBLOCK),"filling real send buffer failed");break;}
            connection.response="pending";
            const auto begin=Clock::now();connection.sendIdleDeadline=begin+std::chrono::seconds(30);const auto deadline=connection.sendIdleDeadline;
            int waits=0;
            while(true){const auto result=sendResponse(connection);require(connection.sendIdleDeadline==deadline&&connection.sentBytes==0,"EAGAIN changed deadline or progress");if(result==SendResult::CloseConnection)break;require(result==SendResult::NeedWrite,"unexpected send result");++waits;std::this_thread::sleep_for(std::chrono::milliseconds(50));}
            const double elapsed=seconds(Clock::now()-begin);require(elapsed>=29.75&&elapsed<35,"wrong real timeout duration");require(drain(pair.fd[1])==std::string(filled,'f'),"unexpected application bytes sent while buffer was full");
            std::cout<<std::fixed<<std::setprecision(6)<<"PASS "<<mode<<" elapsed="<<elapsed<<" real_EAGAIN_returns="<<waits<<"\n";
        }else if(mode=="progress-at-20s"){
            connection.response.resize(1024*1024);for(size_t i=0;i<connection.response.size();++i)connection.response[i]=static_cast<char>('a'+i%26);
            const auto begin=Clock::now();connection.sendIdleDeadline=begin+std::chrono::seconds(30);
            require(sendResponse(connection)==SendResult::NeedWrite&&connection.sentBytes>0,"first real partial send failed");const auto firstDeadline=connection.sendIdleDeadline;
            require(sendResponse(connection)==SendResult::NeedWrite&&connection.sendIdleDeadline==firstDeadline,"initial full-buffer wait failed");
            std::this_thread::sleep_until(begin+std::chrono::seconds(20));
            std::string received=drain(pair.fd[1]);require(received==connection.response.substr(0,connection.sentBytes),"initial response prefix mismatch");const size_t previous=connection.sentBytes;
            require(sendResponse(connection)==SendResult::NeedWrite&&connection.sentBytes>previous,"send did not resume after peer read");const auto renewedDeadline=connection.sendIdleDeadline;require(renewedDeadline-firstDeadline>=std::chrono::milliseconds(19500),"deadline did not move after resumed send");
            require(sendResponse(connection)==SendResult::NeedWrite&&connection.sendIdleDeadline==renewedDeadline,"second EAGAIN extended deadline");
            std::this_thread::sleep_until(firstDeadline+std::chrono::milliseconds(100));require(sendResponse(connection)==SendResult::NeedWrite&&connection.sendIdleDeadline==renewedDeadline,"connection expired at original deadline despite later progress");
            std::this_thread::sleep_until(renewedDeadline+std::chrono::milliseconds(30));require(sendResponse(connection)==SendResult::CloseConnection,"connection did not expire 30 seconds after last progress");const auto finished=Clock::now();
            received+=drain(pair.fd[1]);require(received==connection.response.substr(0,connection.sentBytes),"resumed response bytes/progress mismatch");
            const double elapsed=seconds(finished-begin);require(elapsed>=49.75&&elapsed<55,"renewed deadline real duration mismatch");
            std::cout<<std::fixed<<std::setprecision(6)<<"PASS "<<mode<<" elapsed="<<elapsed<<" idle_after_last_progress="<<seconds(finished-(renewedDeadline-std::chrono::seconds(30)))<<" accepted_bytes="<<connection.sentBytes<<"\n";
        }else throw std::runtime_error("unknown mode");
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<argv[1]<<": "<<error.what()<<"; errno="<<errno<<'\n';return 1;}
}
