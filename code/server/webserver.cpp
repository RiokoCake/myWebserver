#include "webserver.h"

using namespace std;

WebServer::WebServer(
    int port, int trigMode, int timeoutMS, bool OptLinger,
    int sqlPort, const char *sqlUser, const char *sqlPwd,
    const char *dbName, int connPoolNum, int threadNum,
    bool openLog, int logLevel, int logQueSize) : port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
                                                  timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller())
{
    srcDir_ = getcwd(nullptr, 256);
    assert(srcDir_);
    strncat(srcDir_, "/resources/", 16);
    HttpConn::userCount = 0;
    HttpConn::srcDir = srcDir_;
    // 初始化数据库连接池
    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);
    InitEventMode_(trigMode);
    if (!InitSocket_())
    {
        isClose_ = true;
    }

    if (openLog)
    {
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        if (isClose_)
        {
            LOG_ERROR("========== Server init error!==========");
        }
        else
        {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger ? "true" : "false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                     (listenEvent_ & EPOLLET ? "ET" : "LT"),
                     (connEvent_ & EPOLLET ? "ET" : "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

WebServer::~WebServer()
{
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool();
}

/// @brief 初始化监听fd和通信fd的模式
/// @param trigMode
void WebServer::InitEventMode_(int trigMode)
{
    listenEvent_ = EPOLLRDHUP; // 对端描述符产生一个挂断事件，监听对端是否已经关闭
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;
    switch (trigMode)
    {
    case 0:
        break;
    case 1:
        connEvent_ |= EPOLLET;
        break;
    case 2:
        listenEvent_ |= EPOLLET;
        break;
    case 3:
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    default:
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET);
}

/// @brief 服务启动函数
void WebServer::Start()
{
    int timeMS = -1; // epoll wait timeout == -1 无事件将阻塞
    if (!isClose_)
    {
        LOG_INFO("========== Server start ==========");
    }
    while (!isClose_)
    {
        if (timeoutMS_ > 0)
        {
            timeMS = timer_->GetNextTick();
        }
        int eventCnt = epoller_->Wait(timeMS);
        // 遍历事件数
        for (int i = 0; i < eventCnt; i++)
        {
            // 处理事件
            int fd = epoller_->GetEventFd(i);
            uint32_t events = epoller_->GetEvents(i);
            // 处理监听事件，接受新的客户端连接
            if (fd == listenFd_)
            {
                DealListen_();
            }
            // 出现错误，关闭连接
            else if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                assert(users_.count(fd) > 0);
                CloseConn_(&users_[fd]);
            }
            // 处理读事件
            else if (events & EPOLLIN)
            {
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);
            }
            // 处理写事件
            else if (events & EPOLLOUT)
            {
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);
            }
            else
            {
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

void WebServer::SendError_(int fd, const char *info)
{
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);
    if (ret < 0)
    {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

void WebServer::CloseConn_(HttpConn *client)
{
    assert(client);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    epoller_->DelFd(client->GetFd());
    client->Close();
}

/// @brief 添加客户端
/// @param fd
/// @param addr
void WebServer::AddClient_(int fd, sockaddr_in addr)
{
    assert(fd > 0);
    users_[fd].init(fd, addr);
    if (timeoutMS_ > 0)
    {
        // 把&users_[fd]绑定为&WebServer::CloseConn_的参数形成一个函数适配器 作为 add的第三个std::function<void()>类型的参数
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    SetFdNonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

/// @brief 处理监听操作，接受新的客户端连接
void WebServer::DealListen_()
{
    // 连接客户端信息结构体
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    do
    {
        // 获得连接请求，建立连接
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);
        if (fd <= 0)
        {
            return;
        }
        // 控制最大连接数
        else if (HttpConn::userCount >= MAX_FD)
        {
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        AddClient_(fd, addr);
    } while (listenEvent_ & EPOLLET); // ET模式，处理所有监听的fd
}

void WebServer::DealRead_(HttpConn *client)
{
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}

void WebServer::DealWrite_(HttpConn *client)
{
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn *client)
{
    assert(client);
    if (timeoutMS_ > 0)
    {
        timer_->adjust(client->GetFd(), timeoutMS_);
    }
}

void WebServer::OnRead_(HttpConn *client)
{
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno);
    if (ret <= 0 && readErrno != EAGAIN)
    {
        CloseConn_(client);
        return;
    }
    OnProcess(client);
}

/// @brief 处理业务逻辑并更改fd的状态（可读/可写）
/// @param client
void WebServer::OnProcess(HttpConn *client)
{
    if (client->process())
    {
        // 修改对应的文件描述符为可写
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
    }
    else
    {
        // 修改对应的文件描述符为可读
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

void WebServer::OnWrite_(HttpConn *client)
{
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    if (client->ToWriteBytes() == 0)
    {
        /* 传输完成 */
        if (client->IsKeepAlive())
        {
            OnProcess(client);
            return;
        }
    }
    else if (ret < 0)
    {
        if (writeErrno == EAGAIN)
        {
            /* 继续传输 */
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    CloseConn_(client);
}

/// @brief 初始化socket
/// @return 是否创建成功
bool WebServer::InitSocket_()
{
    int ret;
    struct sockaddr_in addr;
    // 限制端口范围
    if (port_ > 65535 || port_ < 1024)
    {
        LOG_ERROR("Port:%d error!", port_);
        return false;
    }
    addr.sin_family = AF_INET; // IPv4

    // 转换为以网络子节序表示的整数以适配TCP协议 APUE-p478
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    struct linger optLinger = {0};
    if (openLinger_)
    {
        // 优雅关闭: 直到所剩数据发送完毕或超时
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }

    // 创建监听fd（IPv4/TCP协议） APUE-p475
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
    {
        LOG_ERROR("Create socket error!", port_);
        return false;
    }
    // 设置套接口的选项
    // SO_LINGER：close或shutdown将等到所有套接字里排队的消息成功发送或到达延迟时间后才会返回
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
    if (ret < 0)
    {
        close(listenFd_);
        LOG_ERROR("Init linger error!", port_);
        return false;
    }

    int optval = 1;
    // SO_REUSEADDR：打开或关闭端口复用功能，当__optval不为0时为打开
    // 只有最后一个套接字会正常接收数据
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));
    if (ret == -1)
    {
        LOG_ERROR("set socket setsockopt error !");
        close(listenFd_);
        return false;
    }

    // 绑定地址和socket
    ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0)
    {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    // 开始监听
    ret = listen(listenFd_, 6);
    if (ret < 0)
    {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    // 向epoller_中添加监听fd
    ret = epoller_->AddFd(listenFd_, listenEvent_ | EPOLLIN);
    if (ret == 0)
    {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }
    // 设置为非阻塞
    SetFdNonblock(listenFd_);
    LOG_INFO("Server port:%d", port_);
    return true;
}

/// @brief 设置文件描述符非阻塞
/// @param fd
/// @return
int WebServer::SetFdNonblock(int fd)
{
    assert(fd > 0);
    int preFlag = fcntl(fd, F_GETFD, 0);
    preFlag |= O_NONBLOCK;
    int curFlag = fcntl(fd, F_SETFL, preFlag);
    return curFlag;
    // return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}
