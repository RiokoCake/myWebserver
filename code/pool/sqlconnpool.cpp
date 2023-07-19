#include "sqlconnpool.h"
using namespace std;

SqlConnPool::SqlConnPool()
{
    useCount_ = 0;
    freeCount_ = 0;
}

SqlConnPool *SqlConnPool::Instance()
{
    static SqlConnPool connPool;
    return &connPool;
}

/// @brief 数据库连接池初始化函数
/// @param host 地址
/// @param port 端口号
/// @param user 用户名
/// @param pwd 密码
/// @param dbName 数据库名称
/// @param connSize 连接池数量
void SqlConnPool::Init(const char *host, int port,
                       const char *user, const char *pwd, const char *dbName,
                       int connSize = 10)
{
    assert(connSize > 0); // 默认连接池数量为10，根据初始化时connSize可更改
    for (int i = 0; i < connSize; i++)
    {
        MYSQL *sql = nullptr;
        // 初始化mysql
        sql = mysql_init(sql);
        if (!sql)
        {
            LOG_ERROR("MySql init error!");
            assert(sql);
        }
        // 连接mysql
        sql = mysql_real_connect(sql, host,
                                 user, pwd,
                                 dbName, port, nullptr, 0);
        if (!sql)
        {
            LOG_ERROR("MySql Connect error!");
        }
        connQue_.push(sql);
    }
    MAX_CONN_ = connSize;
    // 信号量初始化函数 APUE-p468
    int ret = sem_init(&semId_, 0, MAX_CONN_);
    assert(ret == 0);
}

MYSQL *SqlConnPool::GetConn()
{
    MYSQL *sql = nullptr;
    if (connQue_.empty())
    {
        LOG_WARN("SqlConnPool busy!");
        return nullptr;
    }
    sem_wait(&semId_);
    {
        lock_guard<mutex> locker(mtx_);
        sql = connQue_.front();
        connQue_.pop();
    }
    return sql;
}

void SqlConnPool::FreeConn(MYSQL *sql)
{
    assert(sql);
    lock_guard<mutex> locker(mtx_);
    connQue_.push(sql);
    sem_post(&semId_);
}

void SqlConnPool::ClosePool()
{
    lock_guard<mutex> locker(mtx_);
    while (!connQue_.empty())
    {
        auto item = connQue_.front();
        connQue_.pop();
        mysql_close(item);
    }
    mysql_library_end();
}

int SqlConnPool::GetFreeConnCount()
{
    lock_guard<mutex> locker(mtx_);
    return connQue_.size();
}

SqlConnPool::~SqlConnPool()
{
    ClosePool();
}
