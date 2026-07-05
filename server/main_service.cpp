#include "ChatServer.h"
#include "ShardedThreadPool.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void cpuHeavyWork() {
    volatile std::uint64_t result = 0;
    //计算是否质数
    for (std::uint64_t n = 2; n < 100000; ++n) {
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

bool parsePositiveInt(const char* text, int* value) {
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0 || parsed > INT_MAX) {
        return false;
    }

    *value = static_cast<int>(parsed);
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    int worker_count = 16;
    int task_count = 5120;

    if (argc > 3 ||
        (argc >= 2 && !parsePositiveInt(argv[1], &worker_count)) ||
        (argc >= 3 && !parsePositiveInt(argv[2], &task_count))) {
        std::cerr << "usage: " << argv[0] << " [worker_count] [task_count]\n";
        return 1;
    }

    ShardedThreadPool pool(static_cast<std::size_t>(worker_count));
    pool.start();

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