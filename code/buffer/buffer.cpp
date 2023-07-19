#include "buffer.h"

Buffer::Buffer(int initBuffSize) : buffer_(initBuffSize), readPos_(0), writePos_(0) {}

/// @brief 获得可读字节数
size_t Buffer::ReadableBytes() const
{
    return writePos_ - readPos_;
}

/// @brief 获得可写字节数
size_t Buffer::WritableBytes() const
{
    return buffer_.size() - writePos_;
}

size_t Buffer::PrependableBytes() const
{
    return readPos_;
}

const char *Buffer::Peek() const
{
    return BeginPtr_() + readPos_;
}

void Buffer::Retrieve(size_t len)
{
    assert(len <= ReadableBytes());
    readPos_ += len;
}

/// @brief 移动读指针位置
/// @param end
void Buffer::RetrieveUntil(const char *end)
{
    assert(Peek() <= end);
    Retrieve(end - Peek());
}

void Buffer::RetrieveAll()
{
    bzero(&buffer_[0], buffer_.size());
    readPos_ = 0;
    writePos_ = 0;
}

std::string Buffer::RetrieveAllToStr()
{
    std::string str(Peek(), ReadableBytes());
    RetrieveAll();
    return str;
}

const char *Buffer::BeginWriteConst() const
{
    return BeginPtr_() + writePos_;
}

char *Buffer::BeginWrite()
{
    return BeginPtr_() + writePos_;
}

/// @brief 调整可写位置
/// @param len
void Buffer::HasWritten(size_t len)
{
    writePos_ += len;
}

void Buffer::Append(const std::string &str)
{
    Append(str.data(), str.length());
}

void Buffer::Append(const void *data, size_t len)
{
    assert(data);
    Append(static_cast<const char *>(data), len);
}

/// @brief 缓冲区数据添加
/// @param str 临时缓冲区指针
/// @param len 需要添加的数据字节数
void Buffer::Append(const char *str, size_t len)
{
    assert(str);
    EnsureWriteable(len);
    // 把临时缓冲区中的数据拷贝到可写起始位置
    std::copy(str, str + len, BeginWrite());
    HasWritten(len);
}

void Buffer::Append(const Buffer &buff)
{
    Append(buff.Peek(), buff.ReadableBytes());
}

/// @brief 保证可写
/// @param len 需要写的数据长度
void Buffer::EnsureWriteable(size_t len)
{
    if (WritableBytes() < len)
    {
        MakeSpace_(len);
    }
    assert(WritableBytes() >= len);
}

ssize_t Buffer::ReadFd(int fd, int *saveErrno)
{
    char buff[65535];                        // 临时缓冲区，保证读取所有的数据
    struct iovec iov[2];                     // 分散读结构体
    const size_t writable = WritableBytes(); // 可写字节数
    // 分散读， 保证数据全部读完
    iov[0].iov_base = BeginPtr_() + writePos_; // 优先存的位置
    iov[0].iov_len = writable;
    iov[1].iov_base = buff; // 次优先存的位置：防止溢出，放到临时缓冲区中
    iov[1].iov_len = sizeof(buff);

    // 分散读readv APUE-p419
    const ssize_t len = readv(fd, iov, 2);
    if (len < 0)
    {
        *saveErrno = errno;
    }
    else if (static_cast<size_t>(len) <= writable)
    {
        writePos_ += len;
    }
    else
    {
        // 可写缓冲区容量不够，将临时缓冲区的数据添加到可写缓冲区中
        writePos_ = buffer_.size();
        Append(buff, len - writable);
    }
    return len;
}

ssize_t Buffer::WriteFd(int fd, int *saveErrno)
{
    size_t readSize = ReadableBytes();
    ssize_t len = write(fd, Peek(), readSize);
    if (len < 0)
    {
        *saveErrno = errno;
        return len;
    }
    readPos_ += len;
    return len;
}

char *Buffer::BeginPtr_()
{
    return &*buffer_.begin();
}

const char *Buffer::BeginPtr_() const
{
    return &*buffer_.begin();
}

/// @brief 缓冲区扩容
/// @param len
void Buffer::MakeSpace_(size_t len)
{
    if (WritableBytes() + PrependableBytes() < len)
    {
        buffer_.resize(writePos_ + len + 1);
    }
    else
    {
        size_t readable = ReadableBytes();
        // 把当前缓冲区内数据左移至vector的开头，以空出后面位置
        std::copy(BeginPtr_() + readPos_, BeginPtr_() + writePos_, BeginPtr_());
        // 可读位置0
        readPos_ = 0;
        writePos_ = readPos_ + readable;
        assert(readable == ReadableBytes());
    }
}