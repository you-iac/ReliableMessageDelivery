#include "ChatServer.h"
#include "ShardedThreadPool.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void cpuHeavyWork() {
    volatile std::uint64_t result = 0;

    for (std::uint64_t n = 2; n < 200000; ++n) {
        bool prime = true;

        for (std::uint64_t d = 2; d * d <= n; ++d) {
            if (n % d == 0) {
                prime = false;
                break;
            }
        }

        if (prime) {
            result += n;
        }
    }
}

}  // namespace

int main() {
    ShardedThreadPool pool(16);
    pool.start();

    const int task_count = 5120;
    std::atomic<int> finished{0};

    auto begin = std::chrono::steady_clock::now();

    for (int i = 0; i < task_count; ++i) {
        pool.submit(static_cast<std::uintptr_t>(i), [&finished] {
            cpuHeavyWork();
            ++finished;
        });
    }

    while (finished.load() < task_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end = std::chrono::steady_clock::now();

    std::cout << "finished tasks: " << finished.load() << "\n";
    std::cout << "elapsed ms: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count()
              << "\n";

    pool.stop();
    return 0;
}