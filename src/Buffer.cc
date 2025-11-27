#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

#include "Buffer.h"


ssize_t Buffer::readFd(int fd, int *saveErrno)
{
    char extrabuf[65536] = {0};

    struct iovec vec[2];
    const size_t writable = writableBytes();
    vec[0].iov_base = begin() + readerIndex_;
    vec[0].iov_len = writable;

    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);
    
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::read(fd,vec,iovcnt);
    if (n < 0)
    {
        *saveErrno = errno;
    }
    else if (n <= writable) // Buffer的可写缓冲区已经够存储读出来的数据了
    {
        writerIndex_ += n;
    }
    else // extrabuf里面也写入了n-writable长度的数据
    {
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writable); // 对buffer_扩容 并将extrabuf存储的另一部分数据追加至buffer_
    }
    return n;
}


ssize_t Buffer::writeFd(int fd, int *saveErrno)
{
    ssize_t n = ::write(fd,peek(),readableBytes());
    if (n < 0)
    {
        *saveErrno = errno;
    }
    return n;
}