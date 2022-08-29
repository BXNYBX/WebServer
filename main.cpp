#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>  //包含了上面两个  网络通讯
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>   //文件描述符操作
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include <signal.h>

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符到epoll中
extern void addfd( int epollfd, int fd, bool one_shot );
// 从epoll中删除文件描述符
extern void removefd( int epollfd, int fd );
// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发   如果不修改 只会触发一次
extern void modfd(int epollfd, int fd, int ev);

//添加信号捕捉
void addsig(int sig, void( handler )(int)){  //做信号处理的  里面handler 回调函数
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) ); //置空 初始化
    sa.sa_handler = handler;    //回调函数
    sigfillset( &sa.sa_mask );     //将信号集中的所有的标志位置为1
    assert( sigaction( sig, &sa, NULL ) != -1 );  //信号捕捉
}

int main( int argc, char* argv[] ) {  //指定端口号  argc为参数个数
    
    if( argc <= 1 ) {   //至少传递一个端口号 参数
        printf( "usage: %s port_number\n", basename(argv[0])); //basename获取基础名字
        return 1;
    }

    //获取端口号
    int port = atoi( argv[1] );  //转换为整数atoi  argv【0】为程序名
    //对SIGPIPE信号进行处理
    addsig( SIGPIPE, SIG_IGN );  //捕捉信号后 忽略  不管  也不会结束程序

    //创建线程池，初始化线程池
    threadpool< http_conn >* pool = NULL;    //http_conn  为http连接的任务   把任务和客户端信息全放入到http_conn中了
    try {
        pool = new threadpool<http_conn>;  //初始化线程池
    } catch( ... ) {
        return 1;  //捕捉异常  退出
    }

    //创建一个数组用于保存所有的客户端信息  放入http_conn中
    http_conn* users = new http_conn[ MAX_FD ];  //最大的文件描述符个数  (最大客户端)

    //写网络相关代码
    //创建监听的套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 ); //监听文件描述符

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );  //port通过参数获取到

    // 设置端口复用
    int reuse = 1; //1表示复用
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    //绑定
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );

    //监听
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];  // 一次监听的最大的事件数量
    int epollfd = epoll_create( 5 ); //调用epoll_create()创建一个epoll实例  //随便给大于0的值

    // 添加到epoll对象中（调用太多了 写成函数）
    addfd( epollfd, listenfd, false );     //个人理解，不知道对不对，listenfd用于建立新的连接，不能设置为ET或oneshot，否则可能就检测不到后续的新连接了？

    //oneshot指的某socket对应的fd事件最多只能被检测一次，不论你设置的是读写还是异常。因为可能存在这种情况：如果epoll检测到了读事件，
    //数据读完交给一个子线程去处理，如果该线程处理的很慢，在此期间epoll在该socket上又检测到了读事件，则又给了另一个线程去处理，
    //则在同一时间会存在两个工作线程操作同一个socket。ET模式指的是：数据第一次到的时刻才通知，其余时刻不再通知。如果读完了又来了新数据，epoll继续通知。
    //ET模式下可以通知很多次。监听socket不用设置为oneshot是因为只存在一个主线程去操作这个监听socket

    http_conn::m_epollfd = epollfd;  // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的  静态变量再类外定义
    
    while(true) { //循环检测有哪些事情发生
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }
        
        //循环遍历事件数组
        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {
            //有客户端链接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {  
                    //目前连接数满了，告诉服务器正忙
                    //给客户端写一个信息：服务器内部正忙。
                    close(connfd);  //关闭  继续监测
                    continue;
                }

                //将新的客户的数据初始化，放到数组中  文件描述符作为索引来操作
                users[connfd].init( connfd, client_address);

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                //对方异常断开或者错误等事件  关闭连接
                users[sockfd].close_conn();

            } else if(events[i].events & EPOLLIN) {
                //检测读事件
                if(users[sockfd].read()) {
                    //一次性把所有的数据都读完
                    pool->append(users + sockfd);  //交给工作线程去处理
                } else {
                    users[sockfd].close_conn();  //读失败
                }

            }  else if( events[i].events & EPOLLOUT ) {      //检测tcp写缓冲区没有满时  可以写   触发写事件   开始分散写
                    //检测写事件
                if( !users[sockfd].write() ) {
                    //一次性把所有的数据都写完
                    users[sockfd].close_conn();   //写失败
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}