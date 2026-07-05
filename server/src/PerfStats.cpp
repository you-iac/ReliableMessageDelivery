#include "PerfStats.h"

#include <muduo/base/Logging.h>

#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace {

const uint64_t kLogIntervalMs = 1000;

struct StageCounter {
    std::atomic<uint64_t> count;
    std::atomic<uint64_t> total_us;
    std::atomic<uint64_t> max_us;
};

struct StageSnapshot {
    uint64_t count;
    uint64_t total_us;
    uint64_t max_us;
};

StageCounter g_counters[static_cast<int>(PerfStage::Count)];
std::atomic<uint64_t> g_last_log_ms{0};

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            PerfClock::now().time_since_epoch()).count());
}

int StageIndex(PerfStage stage) {
    return static_cast<int>(stage);
}

StageSnapshot Snapshot(PerfStage stage) {
    const StageCounter& counter = g_counters[StageIndex(stage)];
    StageSnapshot snapshot;
    snapshot.count = counter.count.load(std::memory_order_relaxed);
    snapshot.total_us = counter.total_us.load(std::memory_order_relaxed);
    snapshot.max_us = counter.max_us.load(std::memory_order_relaxed);
    return snapshot;
}

double AvgUs(const StageSnapshot& snapshot) {
    if (snapshot.count == 0) {
        return 0.0;
    }
    return static_cast<double>(snapshot.total_us) /
           static_cast<double>(snapshot.count);
}

double PercentOf(const StageSnapshot& child, const StageSnapshot& parent) {
    if (parent.total_us == 0) {
        return 0.0;
    }
    return static_cast<double>(child.total_us) * 100.0 /
           static_cast<double>(parent.total_us);
}

void RecordMax(StageCounter& counter, uint64_t elapsed_us) {
    uint64_t old = counter.max_us.load(std::memory_order_relaxed);
    while (elapsed_us > old &&
           !counter.max_us.compare_exchange_weak(old,
                                                 elapsed_us,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
    }
}

}  // namespace

bool PerfStats::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("RMD_PERF_STATS");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

void PerfStats::Record(PerfStage stage, uint64_t elapsed_us) {
    if (!Enabled()) {
        return;
    }

    StageCounter& counter = g_counters[StageIndex(stage)];
    counter.count.fetch_add(1, std::memory_order_relaxed);
    counter.total_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    RecordMax(counter, elapsed_us);
}

void PerfStats::MaybeLog() {
    if (!Enabled()) {
        return;
    }

    uint64_t now_ms = NowMs();
    uint64_t last_ms = g_last_log_ms.load(std::memory_order_relaxed);
    if (now_ms - last_ms < kLogIntervalMs) {
        return;
    }
    if (!g_last_log_ms.compare_exchange_strong(last_ms,
                                               now_ms,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
        return;
    }

    StageSnapshot chat = Snapshot(PerfStage::HandleChatTotal);
    StageSnapshot chat_create = Snapshot(PerfStage::HandleChatCreate);
    StageSnapshot chat_deliver = Snapshot(PerfStage::HandleChatDeliver);
    StageSnapshot chat_send_ack = Snapshot(PerfStage::HandleChatSendAck);
    StageSnapshot ack = Snapshot(PerfStage::HandleAckTotal);
    StageSnapshot ack_mark = Snapshot(PerfStage::HandleAckMarkAcked);
    StageSnapshot redis_create_lock = Snapshot(PerfStage::RedisCreateLockWait);
    StageSnapshot redis_create_eval = Snapshot(PerfStage::RedisCreateEval);
    StageSnapshot redis_delivered_lock = Snapshot(PerfStage::RedisMarkDeliveredLockWait);
    StageSnapshot redis_delivered_eval = Snapshot(PerfStage::RedisMarkDeliveredEval);
    StageSnapshot redis_acked_lock = Snapshot(PerfStage::RedisMarkAckedLockWait);
    StageSnapshot redis_acked_eval = Snapshot(PerfStage::RedisMarkAckedEval);

    if (chat.count == 0 && ack.count == 0) {
        return;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1)
        << "perf_stats"
        << " chat_count=" << chat.count
        << " chat_avg_us=" << AvgUs(chat)
        << " chat_max_us=" << chat.max_us
        << " create_avg_us=" << AvgUs(chat_create)
        << " create_pct=" << PercentOf(chat_create, chat)
        << " deliver_avg_us=" << AvgUs(chat_deliver)
        << " deliver_pct=" << PercentOf(chat_deliver, chat)
        << " send_ack_avg_us=" << AvgUs(chat_send_ack)
        << " send_ack_pct=" << PercentOf(chat_send_ack, chat)
        << " ack_count=" << ack.count
        << " ack_avg_us=" << AvgUs(ack)
        << " ack_max_us=" << ack.max_us
        << " mark_acked_avg_us=" << AvgUs(ack_mark)
        << " mark_acked_pct=" << PercentOf(ack_mark, ack)
        << " redis_create_lock_avg_us=" << AvgUs(redis_create_lock)
        << " redis_create_eval_avg_us=" << AvgUs(redis_create_eval)
        << " redis_delivered_lock_avg_us=" << AvgUs(redis_delivered_lock)
        << " redis_delivered_eval_avg_us=" << AvgUs(redis_delivered_eval)
        << " redis_acked_lock_avg_us=" << AvgUs(redis_acked_lock)
        << " redis_acked_eval_avg_us=" << AvgUs(redis_acked_eval);

    LOG_INFO << oss.str();
}
