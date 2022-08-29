#ifndef HTTPCONNECTION_H  //如果未定义  下面就定义头文件
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>

class http_conn
{
public:
    static const int FILENAME_LEN = 200;        // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区的大小
    
    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT}; //第一：声明enumType为新的数据类型，称为枚举(enumeration);
    //第二：声明POST, HEAD,等为符号常量，通常称之为枚举量，其值默认分别为0-6。
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    
    // 从状态机的三种可能状态，即行的读取状态，分别表示 
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
public:
    http_conn(){}
    ~http_conn(){}
public:
    void init(int sockfd, const sockaddr_in& addr); // 初始化新接受的连接
    void close_conn();  // 关闭连接
    void process(); // 处理客户端请求（响应也在里面）  任务交给线程（reacter模式 数据在工作线程前就获取了）  先解析http请求  找到资源之后封装成响应信息  再写回去（在主线程写）
                    //只是把要写的数据准备好  检测到可以写时  再写回去
    bool read();// 非阻塞读
    bool write();// 非阻塞写
private:
    void init();    // 初始化连接其余的信息
    HTTP_CODE process_read();    // 解析HTTP请求
    bool process_write( HTTP_CODE ret );    // 填充HTTP应答

    // 下面这一组函数被process_read调用以分析HTTP请求(私有  别人不能访问)
    HTTP_CODE parse_request_line( char* text );     // 解析HTTP请求首行
    HTTP_CODE parse_headers( char* text );          // 解析HTTP请求头
    HTTP_CODE parse_content( char* text );          // 解析HTTP请求体
    HTTP_CODE do_request();                         //做具体处理 解析具体的内容信息                       
    char* get_line() { return m_read_buf + m_start_line; }  //内敛函数 写到一行  获取一行数据
    LINE_STATUS parse_line();               // 解析HTTP请求具体某一行     根据换行解析  相当于获取一行  根据状态交给解析头还是解析体来操作

    // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
    static int m_user_count;    // 统计用户的数量    共享区  连接加1  断开减一

private:
    int m_sockfd;           // 该HTTP连接的socket和对方的socket地址
    sockaddr_in m_address;  // 该HTTP连接的socket和对方的socket地址
    
    char m_read_buf[ READ_BUFFER_SIZE ];    // 读缓冲区  数组
    int m_read_idx;                         // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置

    int m_checked_idx;                      // 当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                       // 当前正在解析的行的起始位置

    CHECK_STATE m_check_state;              // 主状态机当前所处的状态
    METHOD m_method;                        // 请求方法  GET

    char m_real_file[ FILENAME_LEN ];       // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char* m_url;                            // 客户请求的目标文件的文件名
    char* m_version;                        // HTTP协议版本号，我们仅支持HTTP1.1
    char* m_host;                           // 主机名
    int m_content_length;                   // HTTP请求的消息总长度
    bool m_linger;                          // HTTP请求是否要求保持连接

    char m_write_buf[ WRITE_BUFFER_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    //void *iov_base;	/*内存起始位置  */
    //size_t iov_len;	/* 内存长度  */
    int m_iv_count;
};

#endif
