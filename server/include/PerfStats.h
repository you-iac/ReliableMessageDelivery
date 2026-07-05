#ifndef PERF_STATS_H
#define PERF_STATS_H

#include <chrono>
#include <cstdint>

using PerfClock = std::chrono::steady_clock;

enum class PerfStage {
    HandleChatTotal = 0,
    HandleChatCreate,
    HandleChatDeliver,
    HandleChatSendAck,
    HandleAckTotal,
    HandleAckMarkAcked,
    RedisCreateLockWait,
    RedisCreateEval,
    RedisMarkDeliveredLockWait,
    RedisMarkDeliveredEval,
    RedisMarkAckedLockWait,
    RedisMarkAckedEval,
    Count
};

inline uint64_t PerfElapsedUs(PerfClock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            PerfClock::now() - start).count());
}

class PerfStats {
public:
    static void Record(PerfStage stage, uint64_t elapsed_us);
    static void MaybeLog();
    static bool Enabled();
};

#endif
