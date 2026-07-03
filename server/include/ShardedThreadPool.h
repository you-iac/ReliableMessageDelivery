
#include <memory>
#ifndef _ShardedThreadPool
#define macro()

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>


class ShardedThreadPool {
public:
    explicit ShardedThreadPool(size_t worker_count);
    ~ShardedThreadPool();
    using Task = std::function<void()>;
    void start(size_t worker_count);
    void stop();
    
    bool submit(uintptr_t key, Task task);

private:
    struct Worker {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<Task> tasks;
        std::thread thread;
    };

    std::vector<std::unique_ptr<Worker>> workers_;
    bool stopped_ = true;
};

ShardedThreadPool::ShardedThreadPool(size_t worker_count){

    //
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.push_back(std::unique_ptr<Worker>(new Worker()));
        // C++14 才可用
        //workers_.push_back(std::make_unique<Worker>());
    }
}

ShardedThreadPool::~ShardedThreadPool(){
    
}


#endif