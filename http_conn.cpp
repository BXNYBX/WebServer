#include "http_conn.h"

// 定义HTTP响应的一些状态信息   描述
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录  项目的根路径
const char* doc_root = "/home/nybxny/webserver/resources";

// 设置文件描述符非阻塞   边缘ET设置为非阻塞？ IT？ 
int setnonblocking( int fd ) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;  //检测读事件   EPOLLRDHUP异常断开会通过事件判断    水平触发模式  

    //event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;   //设置边沿触发模式
    //个人理解，不知道对不对，listenfd用于建立新的连接，不能设置为ET或oneshot，否则可能就检测不到后续的新连接了？ ET同一时刻到达多个客户怎么坚挺

    if(one_shot) 
    {
        // 防止同一个通信被不同的线程处理
        //视频里的代码里写漏了，将“event.events |= EPOLLONESHOT;”写成了“event.events | EPOLLONESHOT;”所以相当于没有注册成EPOLLONESHOT
        event.events |= EPOLLONESHOT;  //不是对某一个事件的，而是针对某一个socket，也就是文件描述符，如果设置了epolloneshot，
        //那么只会触发一次。防止一个线程在处理业务呢，然后来数据了，又从线程池里拿一个线程来处理新的业务，这样不就乱套了么
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞   边缘ET必须设置为非阻塞 
    setnonblocking(fd);  
}

// 从epoll中移除监听的文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {  //ev为自己事件的值
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event ); // 修改
}

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;  //初始化id 操作
    m_address = addr;   //地址
    
    // 端口复用  断开连接  再次连接可恢复
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    //添加到epoll对象中
    addfd( m_epollfd, sockfd, true );
    m_user_count++;   //总用户数+1
    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              // 客户请求的目标文件的文件名   char
    m_version = 0;          // HTTP协议版本号，我们仅支持HTTP1.1
    m_content_length = 0;    // HTTP请求的消息总长度
    m_host = 0;             // 主机名
    m_start_line = 0;      // 当前正在解析的行的起始位置
    m_checked_idx = 0;     // 当前正在分析的字符在读缓冲区中的位置
    m_read_idx = 0;        // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    m_write_idx = 0;        // 写缓冲区中待发送的字节数
    bzero(m_read_buf, READ_BUFFER_SIZE);  //清空杜缓冲区的数据
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if( m_read_idx >= READ_BUFFER_SIZE ) {  //特殊情况
    //标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置  大于等于  读缓冲区的大小
        return false;
    }
    int bytes_read = 0;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,    //m_read_buf  读缓冲区大小
        READ_BUFFER_SIZE - m_read_idx, 0 ); //从哪读  读的大小
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                // 非阻塞产生的两个错误  没有数据
                break;
            }
            return false;  
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

// 具体解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;  //临时

    //GET / HTTP/1.1\r\n
    //Host: 192.168.91.128:10000\r\n       相当于每行后面都有\r\n

    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) { // 当前正在分析的字符在读缓冲区中的位置  小于 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                return LINE_OPEN;          //行数据尚且不完整
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {
                m_read_buf[ m_checked_idx++ ] = '\0';   //变成字符串结束符，为了获取当前行
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;        //读取到一个完整的行
            }
            return LINE_BAD;          //语法问题
        } else if( temp == '\n' )  {  //可能是前一行数据没有读完 刚好到\r结束 \n就成了第二行的首个字符  因此在检测  上一行返回错误吧
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {  //m_checked_idx > 1     为0的话 减一变负一  不行
                m_read_buf[ m_checked_idx-1 ] = '\0'; 
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    //GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");  // 判断第二个参数中的字符哪个在text中最先出现（一个空格  一个\t）    返回 /index.html HTTP/1.1
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符  /index.html HTTP/1.1  前面无空格了   相当于*m_url='\0'  h和m_url++（位置加一）   改变了txet内容
    char* method = text;  //由于text中有\0  读取到\0结束  为GET

    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;    //表示客户请求语法错误
    }

    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );   // 判断第二个参数中的字符哪个在m_url中最先出现（一个空格  一个\t）
    if (!m_version) {
        return BAD_REQUEST;     //表示客户请求语法错误
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';      // 置位空字符，字符串结束符         改变了m_url指向的内容
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html   （有的中间是这种形式）
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {        //比较7个字符   
        m_url += 7;  //  192.168.110.129:10000/index.html
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );        //  /index.html   
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 主状态机检查状态变成检查请求头
    return NO_REQUEST;         //请求不完整，需要继续读取客户数据
}

// 解析HTTP请求的一个头部信息        理论是对所有头都解析  这里只解析部分
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {              //比如Content-Length：1000  表示有1000个字符的请求体
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;       //没有把请求报文解析完
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } 
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {     // 忽略大小写比较  比较11个字符
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;    //   keep-alive   包含空格
        text += strspn( text, " \t" );   
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;     // HTTP请求是否要求保持连接   设置为保持连接
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段 Content-Length：1000
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);   //变整形
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段  Host: 192.168.91.128:10000
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) { 
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )      // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置 
    //大于等于 HTTP请求的消息总长度+当前正在分析的字符在读缓冲区中的位置
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {    //返回值加作用域
    //初始状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;  //解析结果
    //获取的一行数据
    char* text = 0;
    //循环一行一行解析
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) //m_check_state 当前解析体变为内容且正确行     先执行第一个
                || ((line_status = parse_line()) == LINE_OK)) {       //或者解析一行        当为请球体时 没有\r\n 无法判断  只能用第一个
        //解析到了一行完整的数据，或者解析到了请求体，也是完整数据
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;  //int m_checked_idx;   当前正在分析的字符在读缓冲区中的位置   int m_start_line;  当前正在解析的行的起始位置

        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: 
            {
                ret = parse_request_line( text );  //获取一行数据  交给请求行处理  根据解析情况 返回结果
                if ( ret == BAD_REQUEST ) {   //表示客户请求语法错误    还有多种错误
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );   //获取一行数据  交给请求头处理  根据解析情况 返回结果
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {  //获取了完整的客户端请求   头部解决完就可以这样认为请求完       请球体长度为0 也就结束了
                    return do_request();           //做具体处理 解析具体的内容信息
                }
                break;
            }
            case CHECK_STATE_CONTENT: {        
                ret = parse_content( text );     //获取一行数据  交给请求体处理  根据解析情况 返回结果
                if ( ret == GET_REQUEST ) {       //应该是请求体处理完吧
                    return do_request();          //做具体处理 解析具体的内容信息  url解析出来
                }
                line_status = LINE_OPEN;      //行数据尚不完整
                break;
            }
            default: {
                return INTERNAL_ERROR;         //服务器内部错误
            }
        }
    }
    return NO_REQUEST;         //请求不完整，需要继续读取客户数据    全部解析完  才返回 GET_REQUEST  
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，（GET_REQUEST）  -相当于在服务器本地找到资源html  再写给客户端
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()        //做具体处理 解析具体的内容信息
{
    // "/home/nybxny/webserver/resources"
    strcpy( m_real_file, doc_root );  //拷贝
    int len = strlen( doc_root );     
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );     // m_url为获取的 /index.html   拼接  得到一个真实文件
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {            //相关的状态信息存入结构体m_file_stat中
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {           //获取文件模式（st_mode）  File mode   判断有无 读 访问权限
        return FORBIDDEN_REQUEST;                          //表示客户对资源没有足够的访问权限
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;                               // 如果不是目录 不能把目录资源给 表示客户请求语法错误 （不能是目录）
    } 

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );      // 客户请求的目标文件被mmap到内存中的起始位置   可以发送给客户端
    close( fd );
    return FILE_REQUEST;       //文件请求,获取文件成功        请求操作基本完成
}

// 对内存映射区执行munmap操作（释放）     
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
} 

// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;  // 将要发送的字节  （m_write_idx）写缓冲区中待发送的字节数
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {    //是否保持连接
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

// 往写缓冲中写入待发送的数据（响应数据）
bool http_conn::add_response( const char* format, ... ) {       //   第一个为格式        ...为可变参数
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list; //用来解析参数的  列表
    va_start( arg_list, format );  //传入列表和格式
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );   //m_write_buf + m_write_idx为每次的写的位置（索引）
    //WRITE_BUFFER_SIZE - 1 - m_write_idx为剩余缓冲区大小
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {   //写不下
        return false;
    }
    m_write_idx += len;   //当前已经写完的索引加上len
    va_end( arg_list );  //真正写入进去了  写进m_write_buf数组中
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {            //添加响应状态行（首行）  status为状态码   title为状态吗描述 HTTP/1.1 200 OK
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {           //添加响应头  简化了  有些头没有  拼接每个相应的头
    add_content_length(content_len);
    add_content_type();          //发送回的内容  对应的文件类型很多
    add_linger();                   //是否保持连接
    add_blank_line();         //空行
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );  //是否保持连接
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");    //Content-Type:对应的文件类型很多  还有mp3形式的
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:       //表示服务器内部错误
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:           //表示客户请求语法错误
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:           //请求不完整，需要继续读取客户数据
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:        //表示客户对资源没有足够的访问权限
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:         //文件请求,获取文件成功
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);   //找到后 统计长度
            m_iv[ 0 ].iov_base = m_write_buf;    //内存起始位置
            m_iv[ 0 ].iov_len = m_write_idx;      // 内存长度 
            m_iv[ 1 ].iov_base = m_file_address;   //第二快内存 内存映射的
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;         //通过write函数写
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
// 业务逻辑：！！当有数据来时，一次性把数据读完，读完会调用线程池把任务追加到线程池中（pool->append），线程池执行run函数，不断循环从队列去取， 取出一个任务， 执行process函数，
// 线程池把读到的数据解析，生成响应数据，检测到有数据可写的时候，把数据写出。
void http_conn::process() {
    // 解析HTTP请求（解析请求行 请求头）  状态
    HTTP_CODE read_ret = process_read();                //文件请求,获取文件成功        请求操作基本完成
    if ( read_ret == NO_REQUEST ) {  //请求不完整，需要继续读取客户数据
        modfd( m_epollfd, m_sockfd, EPOLLIN );  //修改 保证in事件能再次检测到
        return;  //process结束  线程空闲
    }
    
    // 生成响应（准备好数据，当检测到可以写时， 直接把准备好的写）
    bool write_ret = process_write( read_ret );     // 两块儿数据  一块是相应头 行 体  还有一块是真正要请求文件的数据  使用分散写写出去
    if ( !write_ret ) {    //返回值响应未成功
        close_conn();   // 关闭一个连接
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);  //重新监听可以写的事件
}