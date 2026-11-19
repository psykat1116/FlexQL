#include "worker/worker.h"

#include <exception>
#include <iostream>

ThreadPool::ThreadPool(size_t num_threads)
{
    for (size_t i = 0; i < num_threads; i++)
    {
        workers.emplace_back([this] {
            while (true)
            {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lk(queue_mutex);
                    cv.wait(lk, [this] { 
                        return stop || !tasks.empty(); 
                    });
                    
                    if (stop && tasks.empty())
                        return;
                    task = std::move(tasks.front());
                    tasks.pop();
                }

                // An exception escaping a std::thread calls std::terminate()
                // and kills the whole server; contain it to this one task.
                try
                {
                    task();
                }
                catch (const std::exception &ex)
                {
                    std::cerr << "[pool] task threw exception: " << ex.what() << "\n";
                }
                catch (...)
                {
                    std::cerr << "[pool] task threw unknown exception\n";
                }
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    {
        // Set stop under the mutex: otherwise a worker can check the wait
        // predicate (stop == false), then miss notify_all before it sleeps,
        // and join() below blocks forever.
        std::lock_guard<std::mutex> lk(queue_mutex);
        stop = true;
    }
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
