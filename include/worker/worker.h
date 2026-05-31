#pragma once
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <condition_variable>

class ThreadPool
{
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    void enqueue(std::function<void()> task);

private:
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
};
