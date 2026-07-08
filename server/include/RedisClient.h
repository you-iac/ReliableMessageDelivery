#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct redisContext;
struct redisReply;

// hiredis 的 redisReply 需要通过 freeReplyObject() 释放。
// 用 unique_ptr 包装后，调用方只需要持有 RedisReplyPtr 即可。
struct RedisReplyDeleter {
    void operator()(redisReply* reply) const;
};

using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

// RedisClient 封装 Redis 命令执行和连接复用。
// 它内部维护一个同步连接池；调用方提交命令参数，RedisClient 负责借连接、
// 执行 hiredis 命令、处理坏连接并返回 reply。
class RedisClient {
public:
    // pool_size 表示最多同时借出的 Redis 同步连接数；0 会在实现中退化为 1。
    explicit RedisClient(std::size_t pool_size = 16);

    // 析构时会等待已借出的连接归还，再释放底层 redisContext。
    ~RedisClient();

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    // 执行任意 Redis 命令。argv[0] 是命令名，后续元素是命令参数。
    // 失败或连接不可用时返回空 RedisReplyPtr。
    RedisReplyPtr command(const std::vector<std::string>& argv);

    // 执行 Lua 脚本。keys 会作为 EVAL 的 KEYS，args 会作为 ARGV。
    RedisReplyPtr eval(const char* script,
                       std::size_t script_len,
                       const std::vector<std::string>& keys,
                       const std::vector<std::string>& args);

private:
    // redisContext 由 RedisClient 独占拥有，用 unique_ptr 自动释放。
    struct RedisContextDeleter {
        void operator()(redisContext* context) const;
    };

    using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;

    struct Slot {
        RedisContextPtr context;
    };

    // Lease 表示从池中借出的一次连接使用权。
    // 析构时自动归还槽位；close() 只在连接损坏时关闭当前槽位的 redisContext。
    class Lease {
    public:
        Lease();
        ~Lease();

        Lease(Lease&& other);
        Lease& operator=(Lease&& other);

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        redisContext* get() const;
        void close();

    private:
        friend class RedisClient;

        Lease(RedisClient* client,
              std::size_t slot_index,
              redisContext* context);

        void release();

        RedisClient* client_;
        std::size_t slot_index_;
        redisContext* context_;
    };

    Lease acquire();
    redisContext* ensureConnected(std::size_t slot_index);
    void closeSlot(std::size_t slot_index);
    void releaseSlot(std::size_t slot_index);

    std::vector<Slot> slots_;
    std::vector<std::size_t> available_;
    std::size_t borrowed_;
    std::mutex mutex_;
    std::condition_variable available_cv_;
    bool stopping_;
};

#endif
