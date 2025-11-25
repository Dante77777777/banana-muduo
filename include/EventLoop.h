#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"

class Channel;
class Poller;

// 事件循环类 主要包含了两个大模块 Channel Poller(epoll的抽象)
class EventLoop : noncopyable
{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    // 开启事件循环
    
    // 退出事件循环
    

    

    // 在当前loop中执行
    
    // 把上层注册的回调函数cb放入队列中 唤醒loop所在的线程执行cb
    
    // 通过eventfd唤醒loop所在的线程
    

    // EventLoop的方法 => Poller的方法
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
   
    // 判断EventLoop对象是否在自己的线程里
     // threadId_为EventLoop创建时的线程id CurrentThread::tid()为当前线程id


};