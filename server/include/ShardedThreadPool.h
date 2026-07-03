
#ifndef _ShardedThreadPool
#define macro()

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>



class ShardedThreadPool {
public:
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
#endif