#include "worker/worker.h"

ThreadPool::ThreadPool(size_t num_threads = std::thread::hardware_concurrency())
{
    for (size_t i = 0; i < num_threads; i++)
    {
        workers.emplace_back([this]
                             {
            while (true)
            {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lk(queue_mutex);
                    cv.wait(lk, [this] { return stop || !tasks.empty(); });
                    if (stop && tasks.empty())
                        return;
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task();
            } });
    }
}

ThreadPool::~ThreadPool()
{
    stop = true;
    cv.notify_all();
    for (auto &t : workers)
        t.join();
}

void ThreadPool::enqueue(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lk(queue_mutex);
        tasks.push(std::move(task));
    }
    cv.notify_one();
}
