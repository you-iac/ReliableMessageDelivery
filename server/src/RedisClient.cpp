#include "RedisClient.h"

#include <cstdlib>

#include <sys/time.h>

#include <hiredis/hiredis.h>

namespace {

const char kDefaultRedisHost[] = "127.0.0.1";
const int kDefaultRedisPort = 6379;

const char* RedisHost() {
    const char* host = std::getenv("RMD_REDIS_HOST");
    return (host != nullptr && host[0] != 0) ? host : kDefaultRedisHost;
}

int RedisPort() {
    const char* text = std::getenv("RMD_REDIS_PORT");
    if (text == nullptr || text[0] == 0) {
        return kDefaultRedisPort;
    }

    char* end = nullptr;
    long port = std::strtol(text, &end, 10);
    if (end == nullptr || *end != 0 || port <= 0 || port > 65535) {
        return kDefaultRedisPort;
    }

    return static_cast<int>(port);
}

redisContext* ConnectRedis() {
    timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 500000;

    redisContext* context = redisConnectWithTimeout(RedisHost(),
                                                    RedisPort(),
                                                    timeout);
    if (context == nullptr) {
        return nullptr;
    }
    if (context->err != 0) {
        redisFree(context);
        return nullptr;
    }
    return context;
}

bool IsConnectionBroken(const redisContext* context, const redisReply* reply) {
    return context == nullptr || context->err != 0 || reply == nullptr;
}

}  // namespace

void RedisReplyDeleter::operator()(redisReply* reply) const {
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
}

void RedisClient::RedisContextDeleter::operator()(redisContext* context) const {
    if (context != nullptr) {
        redisFree(context);
    }
}

RedisClient::Lease::Lease()
    : client_(nullptr),
      slot_index_(0),
      context_(nullptr) {
}

RedisClient::Lease::Lease(RedisClient* client,
                          std::size_t slot_index,
                          redisContext* context)
    : client_(client),
      slot_index_(slot_index),
      context_(context) {
}

RedisClient::Lease::~Lease() {
    release();
}

RedisClient::Lease::Lease(Lease&& other)
    : client_(other.client_),
      slot_index_(other.slot_index_),
      context_(other.context_) {
    other.client_ = nullptr;
    other.context_ = nullptr;
}

RedisClient::Lease& RedisClient::Lease::operator=(Lease&& other) {
    if (this != &other) {
        release();
        client_ = other.client_;
        slot_index_ = other.slot_index_;
        context_ = other.context_;
        other.client_ = nullptr;
        other.context_ = nullptr;
    }
    return *this;
}

redisContext* RedisClient::Lease::get() const {
    return context_;
}

void RedisClient::Lease::close() {
    if (client_ == nullptr) {
        return;
    }
    client_->closeSlot(slot_index_);
    context_ = nullptr;
}

void RedisClient::Lease::release() {
    if (client_ == nullptr) {
        return;
    }
    client_->releaseSlot(slot_index_);
    client_ = nullptr;
    context_ = nullptr;
}

RedisClient::RedisClient(std::size_t pool_size)
    : slots_(pool_size == 0 ? 1 : pool_size),
      borrowed_(0),
      stopping_(false) {
    available_.reserve(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        available_.push_back(slots_.size() - i - 1);
    }
}

RedisClient::~RedisClient() {
    std::unique_lock<std::mutex> lock(mutex_);
    stopping_ = true;
    available_cv_.notify_all();
    while (borrowed_ > 0) {
        available_cv_.wait(lock);
    }
}

RedisClient::Lease RedisClient::acquire() {
    std::size_t slot_index = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (available_.empty() && !stopping_) {
            available_cv_.wait(lock);
        }
        if (stopping_) {
            return Lease();
        }

        slot_index = available_.back();
        available_.pop_back();
        ++borrowed_;
    }

    return Lease(this, slot_index, ensureConnected(slot_index));
}

redisContext* RedisClient::ensureConnected(std::size_t slot_index) {
    redisContext* context = slots_[slot_index].context.get();
    if (context != nullptr && context->err == 0) {
        return context;
    }

    closeSlot(slot_index);
    slots_[slot_index].context.reset(ConnectRedis());
    return slots_[slot_index].context.get();
}

void RedisClient::closeSlot(std::size_t slot_index) {
    slots_[slot_index].context.reset();
}

void RedisClient::releaseSlot(std::size_t slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (borrowed_ > 0) {
            --borrowed_;
        }
        if (!stopping_) {
            available_.push_back(slot_index);
        }
    }
    available_cv_.notify_one();
}

RedisReplyPtr RedisClient::command(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return RedisReplyPtr();
    }

    Lease connection = acquire();
    redisContext* redis = connection.get();
    if (redis == nullptr) {
        return RedisReplyPtr();
    }

    std::vector<const char*> argv_data;
    std::vector<std::size_t> argv_len;
    argv_data.reserve(argv.size());
    argv_len.reserve(argv.size());

    for (std::size_t i = 0; i < argv.size(); ++i) {
        argv_data.push_back(argv[i].data());
        argv_len.push_back(argv[i].size());
    }

    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommandArgv(redis,
                         static_cast<int>(argv.size()),
                         argv_data.data(),
                         argv_len.data())));

    if (IsConnectionBroken(redis, reply.get())) {
        connection.close();
        return RedisReplyPtr();
    }

    return reply;
}

RedisReplyPtr RedisClient::eval(const char* script,
                                std::size_t script_len,
                                const std::vector<std::string>& keys,
                                const std::vector<std::string>& args) {
    if (script == nullptr || script_len == 0) {
        return RedisReplyPtr();
    }

    std::vector<std::string> argv;
    argv.reserve(3 + keys.size() + args.size());
    argv.push_back("EVAL");
    argv.push_back(std::string(script, script_len));
    argv.push_back(std::to_string(keys.size()));
    argv.insert(argv.end(), keys.begin(), keys.end());
    argv.insert(argv.end(), args.begin(), args.end());

    return command(argv);
}
