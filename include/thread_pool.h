#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <cstddef>

class ThreadPool {
public:
    explicit ThreadPool(int numThreads,std::size_t maxQueuedTasks); //显式构造线程池，指定工作线程数和等待队列容量
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    //class F: 声明一个类型模板参数F，class... Args: 可变参数模板，Args是一个类型包，表示可接受任意个类型
    template<class F,class... Args> //template<..>: 表示接下来定义的是一个模板
    auto enqueue(F&& f, Args&&... args)//F&& :转发引用，可同时接受左右值，Args&&同理可接任意数量参数
    {

        auto boundTask = [
                    //std::forward<T>(x) 的作用是：在模板函数中，把参数按它原来的值类别（左值 / 右值）继续传递下去，保持完美转发
                    func = std::forward<F>(f), //把用户函数保存为func

                    //c++14中初始化不需要声明
                    args_tuple = std::make_tuple( //把参数包展开、decay、装进 tuple
                        std::forward<Args>(args)...
                    )
                    /*普通 lambda 默认会把返回的引用推导成值。例如用户函数返回 int&，默认推导可能变成 int。
                    decltype(auto) 可以保留这个引用*/
                ]() mutable -> decltype(auto){//mutable: 允许修改捕获的成员

                    return std::apply( //std::apply: 把一个 tuple 拆开，作为参数传给一个可调用对象(func)。
                        func,
                        std::move(args_tuple)
                    ); //相当于返回func(第一个参数, 第二个参数, ..., 第N个参数);结果被packaged_task捕获，调用 future.get() 时拿到

                };

        using ReturnType =
                                //decltype(boundTask)：获取这个任务 lambda 的类型,后面的 &：按任务对象是左值的方式检查调用
            std::invoke_result_t<decltype(boundTask)&>; //invoke_result<>: 推导返回值类型，如int add(int,int)则using ReturnType=int;（即函数前的类型）

        auto task =
            //std::make_shared 创建的是一个 shared_ptr 智能指针，
            //这个指针指向一个动态创建的 packaged_task 对象，让后续代码可以通过这个指针访问同一个 packaged_task
            std::make_shared<
                std::packaged_task<ReturnType()> //packaged_task<>: 保存函数，并保存函数返回值的任务包装器,最后由future获取
            >(
                std::move(boundTask)
            );

        std::future<ReturnType> result =
            task->get_future();         // packaged_task保存结果后由future得到结果
        {
            std::unique_lock<std::mutex> lock(mtx); // 加锁保护任务队列

            if(stop) // 检查线程池状态，如果线程池已关闭则不能添加任务
            {
                throw std::runtime_error(
                    "ThreadPool has stopped"
                );
            }

            //入队之前检查容量是否足够
            if(tasks.size() >= maxQueuedTasks_){
                throw std::runtime_error("ThreadPool task queue is full");
            }

            tasks.emplace( // 加入任务队列
                // [task]: 按值捕获,把shared_ptr 拷贝一份到 lambda 内部
                [task]()
                {
                    (*task)();  /*  task 是 shared_ptr；
                                    *task 解引用，得到 packaged_task<ReturnType()>&；
                                    (*task)() 调用 packaged_task::operator()执行内部lambda */
                }
            );

        }


        condition.notify_one(); // 通知线程


        return result; // 返回future
    }
private:
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> tasks;

    std::mutex mtx;
    std::condition_variable condition;

    bool stop = false;

    std::size_t maxQueuedTasks_; //保存等待队列的容量上限
};
