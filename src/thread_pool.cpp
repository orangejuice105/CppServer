#include "thread_pool.h"

ThreadPool::ThreadPool(int numThreads) {
    if(numThreads <= 0){
            throw std::invalid_argument("numThreads must be greater than 0");
        }

        // 在线程启动前分配容器容量
        threads.reserve(static_cast<std::size_t>(numThreads)); //static_cast<size_t>: 类型转换为size_t

        try {
            for (int i = 0; i < numThreads; i++) {
                threads.emplace_back([this] {
                    while (true) {
                        std::unique_lock<std::mutex> lock(mtx);

                        condition.wait(lock, [this] {
                            return !tasks.empty() || stop;
                        });

                        if (stop && tasks.empty()) {
                            return;
                        }

                        std::function<void()> task(std::move(tasks.front()));
                        tasks.pop();

                        lock.unlock();
                        task();
                    }
                });
            }
        }
        catch (...) {
            {
                std::unique_lock<std::mutex> lock(mtx);
                stop = true;
            }

            condition.notify_all();

            for (auto& t : threads) {
                if (t.joinable()) {
                    t.join();
                }
            }

            throw;
        }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop = true;
    }

    condition.notify_all();
    for(auto& t : threads){
        t.join();
    }
}
