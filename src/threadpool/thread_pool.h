#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../MySqlPool/sql_conn_pool.h"

template <typename T>
class threadpool
{
public:
    /**
     * @param actor_model 模型切换类型
     * @param conn_pool 数据库连接池
     * @param thread_number 线程池中线程的数量
     * @param max_request 请求队列中最多允许的等待处理的请求的数量
     */
    threadpool(int actor_model, connection_pool *conn_pool, int thread_number = 8, int max_request = 10000);
    ~threadpool();

    /**
     * @brief 向请求队列添加请求
     * @param request 请求指针
     * @param state 请求状态
     * @return 添加是否成功
     */
    bool append(T *request, int state);

    /**
     * @brief 向请求队列添加请求（无状态）
     * @param request 请求指针
     * @return 添加是否成功
     */
    bool append_p(T *request);

private:
    /**
     * @brief 工作线程运行的函数，不断从工作队列中取出任务并执行
     * @param arg 线程池指针
     * @return 线程池指针
     */
    static void *worker(void *arg);
    /**
     * @brief 线程工作函数，执行任务处理
    */
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;//数据库
    int m_actor_model;          //模型切换
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if(thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads)
        throw std::exception();
    for(int i = 0; i < thread_number; ++i){
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){
            delete[] m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i])){
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if(m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if(m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait(); // 等待是否有任务需要处理
        m_queuelocker.lock(); // 获取请求队列的互斥锁
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock(); // 如果队列为空，释放互斥锁并继续等待
            continue;
        }
        T *request = m_workqueue.front(); // 获取队列中的第一个请求
        m_workqueue.pop_front(); // 从队列中移除该请求
        m_queuelocker.unlock(); // 释放互斥锁

        if (!request)
            continue;

        // 根据线程池的模型类型（m_actor_model），执行不同的处理逻辑
        if (1 == m_actor_model)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    connection_raii mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            connection_raii mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif
