#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>
class ThreadPool
{
public:
    explicit ThreadPool(size_t threadCount = 8) : pool_(std::make_shared<Pool>())
    {
        assert(threadCount > 0);
        for (size_t i = 0; i < threadCount; i++)
        {
            std::thread([pool = pool_]
                        {
                    // 互斥锁包装器，创建即加锁
                    // 如需延迟加锁则在初始化时加std::defer_lock，即locker(pool->mtx, std::defer_lock)
                    std::unique_lock<std::mutex> locker(pool->mtx);
                    while(true) {
                        if(!pool->tasks.empty()) {
                            // 获取任务队列的第一个任务对象
                            auto task = std::move(pool->tasks.front());
                            pool->tasks.pop();
                            // 互斥锁解锁
                            locker.unlock();
                            // 执行具体任务
                            task();
                            // 任务结束，互斥锁加锁
                            locker.lock();
                        }
                        // 如果已关闭，退出循环
                        else if(pool->isClosed) break;
                        else pool->cond.wait(locker);
                    } })
                .detach(); // 分离主、子线程，保证子线程不受主线程结束影响
        }
    }

    ThreadPool() = default;

    ThreadPool(ThreadPool &&) = default;

    ~ThreadPool()
    {
        if (static_cast<bool>(pool_))
        {
            {
                std::lock_guard<std::mutex> locker(pool_->mtx);
                pool_->isClosed = true;
            }
            pool_->cond.notify_all();
        }
    }
    /// @brief 线程池添加任务
    /// @tparam T
    /// @param task
    template <class T>
    void AddTask(T &&task)
    {
        {
            std::lock_guard<std::mutex> locker(pool_->mtx);
            pool_->tasks.emplace(std::forward<T>(task));
        }
        pool_->cond.notify_one();
    }

private:
    // 线程池结构体
    struct Pool
    {
        std::mutex mtx;                          // 互斥锁
        std::condition_variable cond;            // 条件变量
        bool isClosed;                           // 是否已关闭标志
        std::queue<std::function<void()>> tasks; // 任务队列，队列中存储函数包装器std::function包装具体任务
    };
    std::shared_ptr<Pool> pool_;
};

#endif // THREADPOOL_H