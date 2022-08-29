#ifndef THREADPOOL_H  //线程池
#define THREADPOOL_H

#include <list> //队列
#include <cstdio>//C++ print
#include <exception> //错误
#include <pthread.h>  //线程
#include "locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool {
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);  //添加任务

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void* worker(void* arg); //静态函数不能访问非晶态成员变量  相当于全局函数
    void run();

private:
    // 线程的数量
    int m_thread_number;  
    
    // 描述线程池的数组，大小为m_thread_number    
    pthread_t * m_threads;     //动态创建数组

    // 请求队列中最多允许的、等待处理的请求的数量  
    int m_max_requests; 
    
    // 请求队列
    std::list< T* > m_workqueue;  //命名空间

    // 保护请求队列的互斥锁
    locker m_queuelocker;   

    // 是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程          
    bool m_stop;                    
};

template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) : 
        m_thread_number(thread_number), m_max_requests(max_requests), 
        m_stop(false), m_threads(NULL) {

    if((thread_number <= 0) || (max_requests <= 0) ) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number]; //创建数组
    if(!m_threads) {
        throw std::exception();
    }

    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for ( int i = 0; i < thread_number; ++i ) {
        printf( "create the %dth thread\n", i);
        if(pthread_create(m_threads + i, NULL, worker, this ) != 0) {   //m_threads + i为线程好 返回参数  本身也要传入地址  worker为子线程执行的代码（静态函数）
            delete [] m_threads;                                        //this 代表本类对象 作为参数传入worker函数中  函数中就可以拿到this了
            throw std::exception();
        }
        
        if( pthread_detach( m_threads[i] ) ) {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template< typename T >
threadpool< T >::~threadpool() {
    delete [] m_threads;
    m_stop = true;
}

template< typename T >
bool threadpool< T >::append( T* request )
{
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests ) {   //超出了最大量 
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();         //信号量加一  队列增加一个  根据信号量判断
    return true;
}

template< typename T >
void* threadpool< T >::worker( void* arg )
{
    threadpool* pool = ( threadpool* )arg;   //this指针的转换   C++的类成员函数都有一个默认参数 this 指针，而线程调用的时候，限制了只能有一个参数 void* arg，
                                            //如果不设置成静态在调用的时候会出现this 和arg都给worker 导致错误。个人理解~
    pool->run();
    return pool;
}

template< typename T >
void threadpool< T >::run() {  //线程值运行  

    while (!m_stop) {  //循环  为false一直执行
        m_queuestat.wait();  //信号量不为0 有值 不阻塞  减一
        m_queuelocker.lock();
        if ( m_workqueue.empty() ) {      //继续看队列有无数据
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();   //获取第一个任务
        m_workqueue.pop_front();
        m_queuelocker.unlock();  //解完锁  去执行
        if ( !request ) {
            continue;
        }
        request->process(); //在http——conn中实现
    }

}

#endif

/*
不理解为什么用互斥锁和信号量的同学可以看一下，其实这就是个单生产者多消费者的生产者消费者模型。
主线程是生产者，线程池中的子线程是消费者，互斥锁是解决互斥问题的，信号量是解决同步问题的。
互斥关系体现在：任务请求队列其实是共享资源，主线程将新任务加入到队列中这一操作和若干个子线程从队列中取数据的操作是互斥的。
同步体现在：当任务队列中没有任务时，必须主线程先放入任务，再子线程取任务。主线程append的时候进行了post操作，其实就是执行了同步信号量的V操作，而子线程的wait就是执行了P操作。
*/