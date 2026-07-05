
#include <memory>
#ifndef SHARDED_THREAD_POOL_H
#define SHARDED_THREAD_POOL_H

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>
#include <cstddef>
#include <cstdint>

class ShardedThreadPool {
public:
    explicit ShardedThreadPool(size_t worker_count);
    ~ShardedThreadPool();
    using Task = std::function<void()>;
    //开始执行线程池
    void start();
    //停止线程池
    void stop();
    //提交一个任务到线程池中，key用于选择线程
    bool submit(uintptr_t key, Task task);
    // 获取线程池每个线程的待处理任务数，不包含正在执行的任务。
    std::vector<std::size_t> getQueueSizes() const;

private:
    struct Worker {
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::queue<Task> tasks;
        std::thread thread;
    };

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> stopped_{true};
};

inline ShardedThreadPool::ShardedThreadPool(size_t worker_count){

    //创建多个工作线程
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.push_back(std::unique_ptr<Worker>(new Worker()));
        // C++14 才可用
        //workers_.push_back(std::make_unique<Worker>());
    }
}

inline ShardedThreadPool::~ShardedThreadPool(){
    this->stop();    
}

inline void ShardedThreadPool::start(){
    //必须是从停止状态到开始运行
    if (!stopped_) {
        return;
    }
    //修改运行状态
    this->stopped_.store(false);
    //
    for(size_t i = 0; i < this->workers_.size(); i++){
        Worker* worker = this->workers_[i].get();
        //使用lambeda表达式创建线程
        worker->thread = std::thread([this, worker] {
            
            while (true) {
                Task task;
                {
                    //单个线程访问队列，使用互斥锁保护
                    std::unique_lock<std::mutex> lock(worker->mutex);
                    //条件变量带有条件的等待，避免虚假唤醒
                    worker->cv.wait(lock, [this, worker]() { 
                            return !worker->tasks.empty() || this->stopped_; 
                        });
                    //结束条件，线程池关闭并且队列为空。
                    if (this->stopped_ && worker->tasks.empty()) {
                        return;
                    }

                    task = std::move(worker->tasks.front());
                    worker->tasks.pop();
                }
                task();
            }
        });
    }
}

inline bool ShardedThreadPool::submit(uintptr_t key, Task task){
    //线程池非停止并且非空。
    if (stopped_ || workers_.empty()) {
        return false;
    }
    // 指针类 key 通常低位为 0，直接取模会让任务集中到 worker 0。
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;

    //获取工作分片
    Worker*  w = workers_[key % this->workers_.size()].get();
    
    {

        std::lock_guard<std::mutex> lock(w->mutex);
         if (stopped_) {
            return false;
        }
        w->tasks.push(std::move(task));
    }

    w->cv.notify_one();
    return true;
}

inline std::vector<std::size_t> ShardedThreadPool::getQueueSizes() const{
    std::vector<std::size_t> sizes;
    sizes.reserve(workers_.size());

    for (const auto& worker : workers_) {
        std::lock_guard<std::mutex> lock(worker->mutex);
        sizes.push_back(worker->tasks.size());
    }

    return sizes;
}

inline void ShardedThreadPool::stop(){
    this->stopped_.store(true);
    for(auto& work : this->workers_){
        work.get()->cv.notify_all();
    }

    for (auto& work : workers_) {
        if (work->thread.joinable()) {
            work->thread.join();
        }
    }
}

#endif