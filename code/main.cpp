#include <unistd.h>
#include "server/webserver.h"

int main()
{
    // daemon(1, 0);    //后台运行

    WebServer server(
        1316,            /* 端口  */
        3,               /* ET模式 */
        60000,           /* timeoutMs */
        false,           /* 优雅退出 */
        3306,            /* 端口 */
        "root", "kyrie", /* 用户名 密码 */
        "yourdb",        /* 数据库名称 */
        12, 6,           /* 连接池数量 线程池数量 */
        true, 1, 1024);  /* 日志开关 日志等级 日志异步队列容量 */
    server.Start();
}