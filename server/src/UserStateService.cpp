#include "UserStateService.h"

#include <chrono>

void UserStateService::addConnection(const muduo::net::TcpConnectionPtr& conn) {
    // 空连接没有可登记的 TcpConnection*，直接忽略。
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // 连接建立时先登记为 Connected，登录成功后再 bindUser。
    UserState& state = conn_states_[conn.get()];
    state.conn = conn;
    state.last_active_ms = nowMs();
}

void UserStateService::removeConnection(
    const muduo::net::TcpConnectionPtr& conn) {
    // 断开回调可能传入空连接，保持接口幂等。
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conn_states_.find(conn.get());
    // 未登记连接无需清理。
    if (it == conn_states_.end()) {
        return;
    }

    // 如果该连接已经绑定 uid，同步清理 uid -> conn 索引。
    // 这里校验 uid_to_conn_ 中的 conn，避免误删用户的新连接。
    if (it->second.uid != 0) {
        auto online_it = uid_to_conn_.find(it->second.uid);
        if (online_it != uid_to_conn_.end() &&
            online_it->second.get() == conn.get()) {
            uid_to_conn_.erase(online_it);
        }
    }

    conn_states_.erase(it);
}

bool UserStateService::bindUser(const muduo::net::TcpConnectionPtr& conn,
                                uint64_t uid) {
    // uid 使用 0 表示未登录，因此不能绑定为有效用户。
    if (!conn || uid == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // 允许在 addConnection 之前绑定，operator[] 会补建连接状态。
    UserState& state = conn_states_[conn.get()];
    state.conn = conn;

    // 同一条连接重复登录时，先移除旧 uid 的在线索引。
    if (state.uid != 0) {
        auto old_online_it = uid_to_conn_.find(state.uid);
        if (old_online_it != uid_to_conn_.end() &&
            old_online_it->second.get() == conn.get()) {
            uid_to_conn_.erase(old_online_it);
        }
    }

    // 第一版采用单端在线策略：同一个 uid 只保留一个连接。
    // 如果新连接登录同一个 uid，旧连接的用户状态会退回 Connected。
    auto existing_user_it = uid_to_conn_.find(uid);
    if (existing_user_it != uid_to_conn_.end() &&
        existing_user_it->second.get() != conn.get()) {
        auto old_state_it = conn_states_.find(existing_user_it->second.get());
        if (old_state_it != conn_states_.end()) {
            old_state_it->second.uid = 0;
            old_state_it->second.state = UserConnectionState::Connected;
        }
    }

    // 更新当前连接的主状态表和 uid 在线索引，二者必须保持一致。
    state.uid = uid;
    state.state = UserConnectionState::Authenticated;
    state.last_active_ms = nowMs();
    uid_to_conn_[uid] = conn;
    return true;
}

uint64_t UserStateService::getUidByConn(
    const muduo::net::TcpConnectionPtr& conn) const {
    // 对外约定：0 表示没有可用的已认证用户。
    if (!conn) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conn_states_.find(conn.get());
    // 只有 Authenticated 的连接才被视为已登录。
    if (it == conn_states_.end() ||
        it->second.state != UserConnectionState::Authenticated) {
        return 0;
    }
    return it->second.uid;
}

muduo::net::TcpConnectionPtr UserStateService::getConnByUid(uint64_t uid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = uid_to_conn_.find(uid);
    // 未找到在线索引时返回空智能指针，调用方据此判断用户离线。
    if (it == uid_to_conn_.end()) {
        return muduo::net::TcpConnectionPtr();
    }
    return it->second;
}

bool UserStateService::isAuthenticated(
    const muduo::net::TcpConnectionPtr& conn) const {
    return getUidByConn(conn) != 0;
}

UserConnectionState UserStateService::getState(
    const muduo::net::TcpConnectionPtr& conn) const {
    // 空连接没有用户绑定关系，按初始状态处理。
    if (!conn) {
        return UserConnectionState::Connected;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conn_states_.find(conn.get());
    // 未登记连接同样视为未认证连接。
    if (it == conn_states_.end()) {
        return UserConnectionState::Connected;
    }
    return it->second.state;
}

void UserStateService::updateHeartbeat(
    const muduo::net::TcpConnectionPtr& conn) {
    // 空连接无法更新活跃时间。
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conn_states_.find(conn.get());
    if (it != conn_states_.end()) {
        // 心跳只刷新活跃时间，不改变登录状态。
        it->second.last_active_ms = nowMs();
    }
}

uint64_t UserStateService::nowMs() {
    using namespace std::chrono;
    // 统一使用毫秒时间戳，方便与心跳超时阈值直接比较。
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());
}
