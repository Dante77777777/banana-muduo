#include <functional>
#include <string>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <fcntl.h> // for open
#include <unistd.h> // for close

#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null!\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string &nameArg,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CheckLoopNotNull(loop))
    , name_(nameArg)
    , state_(kConnecting)
    , reading_(true)
    , socket_(new Socket(sockfd))
    , channel_(new Channel(loop, sockfd))
    , localAddr_(localAddr)
    , peerAddr_(peerAddr)
    , highWaterMark_(64 * 1024 * 1024) // 64M
{
    // 下面给channel设置相应的回调函数 poller给channel通知感兴趣的事件发生了 channel会回调相应的回调函数
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

    LOG_INFO("TcpConnection::ctor[%s] at fd=%d\n", name_.c_str(), sockfd);
    socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::dtor[%s] at fd=%d state=%d\n", name_.c_str(), channel_->fd(), (int)state_);
}

void TcpConnection::send(const std::string &buf)
{
    if(state_ == kConnected)
    {
        if(loop_->isInLoopThread())
        {
            sendInLoop(buf.c_str(),buf.size());
        }
        else
        {
            loop_->runInLoop(std::bind(&TcpConnection::sendInLoop,this,buf.c_str(),buf.size()));
        }
    }
}

void TcpConnection::sendInLoop(const void *data, size_t len)
{
    ssize_t nwrote = 0;
    size_t remain = len;
    bool faultError = false;

    if (state_ == kDisconnected) // 之前调用过该connection的shutdown 不能再进行发送了
    {
        LOG_ERROR("disconnected, give up writing");
        // return ;
    }

    if(!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(),data,len);
        if(nwrote >= 0)
        {
            remain = len - nwrote;
            if(remain == 0 && writeCompleteCallback_)
            {
                loop_->queueInLoop(std::bind(writeCompleteCallback_,shared_from_this()));
            }
        }
        else
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK) // EWOULDBLOCK表示非阻塞情况下没有数据后的正常返回 等同于EAGAIN
            {
                LOG_ERROR("TcpConnection::sendInLoop");
                if (errno == EPIPE || errno == ECONNRESET) // SIGPIPE RESET
                {
                    faultError = true;
                }
            }
        }
    }

    if(remain > 0 && !faultError)
    {
        size_t oldLen = outputBuffer_.readableBytes();
        if(remain + oldLen > highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_)
        {
            loop_->queueInLoop(std::bind(highWaterMarkCallback_,shared_from_this(),remain+oldLen));
        }
        outputBuffer_.append((char*)data+nwrote,remain);
        if(!channel_->isWriting())
        {
            channel_->enableWriting();
        }
    }
}


void TcpConnection::shutdown()
{
    if(state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->queueInLoop(std::bind(&TcpConnection::shutdownInLoop,this));
    }
}


void TcpConnection::shutdownInLoop()
{
    if(!channel_->isWriting())
    {
        socket_->shutdownWrite();
    }
}


void TcpConnection::handleRead(Timestamp receiveTime)
{
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(),&savedErrno);
    if(n > 0)
    {
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
    else if(n == 0)
    {
        handleClose();
    }
    else
    {
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead");
        handleError();
    }
}


void TcpConnection::handleWrite()
{
    if(channel_->isWriting())
    {
        int savedErrno = 0;
        int n = outputBuffer_.writeFd(channel_->fd(),&savedErrno);
        if(n > 0)
        {
            outputBuffer_.retrieve(n);
            if(outputBuffer_.readableBytes() == 0)
            {
                channel_->disableWriting();
                if(writeCompleteCallback_)
                {
                    loop_->queueInLoop(std::bind(writeCompleteCallback_,shared_from_this()));
                }
            }
            if(state_ == kDisconnecting)
            {
                shutdownInLoop();
            }
        }
        else
        {
            LOG_ERROR("TcpConnection::handleWrite");
        }
    }
    else
    {
        LOG_ERROR("TcpConnection fd=%d is down, no more writing", channel_->fd());
    }
}


void TcpConnection::handleClose()
{
    LOG_INFO("TcpConnection::handleClose fd=%d state=%d\n", channel_->fd(), (int)state_);
    setState(kDisconnected);
    channel_->disableAll();
    
    TcpConnectionPtr connPtr(shared_from_this());
    connectionCallback_(connPtr);
    closeCallback_(connPtr);
}


void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name:%s - SO_ERROR:%d\n", name_.c_str(), err);
}


void TcpConnection::sendFile(int fileDescriptor, off_t offset, size_t count)
{
    if(connected())
    {
        if(loop_->isInLoopThread())
        {
            sendFileInLoop(fileDescriptor,offset,count);
        }
        else
        {
            loop_->runInLoop(std::bind(TcpConnection::sendFileInLoop,this,fileDescriptor,offset,count));
        }
    }
    else
    {
        LOG_ERROR("TcpConnection::sendFile - not connected");
    }
}


void TcpConnection::sendFileInLoop(int fileDescriptor, off_t offset, size_t count)
{
    ssize_t bytesSent = 0;
    size_t remain = count;
    bool faultError = false;
    
    if(state_ == kDisconnecting)
    {
        LOG_ERROR("disconnected, give up writing");
        return ;
    }

    if(!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        bytesSent = ::sendfile(socket_->fd(),fileDescriptor,&offset,count);
        if(bytesSent > 0)
        {
            remain -= bytesSent;
            if(remain == 0 && writeCompleteCallback_)
            {
                loop_->queueInLoop(std::bind(writeCompleteCallback_,shared_from_this()));
            }
        }
        else 
        { // bytesSent < 0
            if (errno != EWOULDBLOCK) 
            { // 如果是非阻塞没有数据返回错误这个是正常显现等同于EAGAIN，否则就异常情况
                LOG_ERROR("TcpConnection::sendFileInLoop");
            }
            if (errno == EPIPE || errno == ECONNRESET) 
            {
                faultError = true;
            }
        }
    }
    if(!faultError && remain > 0)
    {
        loop_->queueInLoop(std::bind(&TcpConnection::sendFileInLoop,shared_from_this(),fileDescriptor,offset,remain));
    }
}