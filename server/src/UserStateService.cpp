#include "UserStateService.h"

#include <chrono>

void UserStateService::addConnection(const muduo::net::TcpConnectionPtr& conn) {
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
    if (!conn) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conn_states_.find(conn.get());
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
    if (!conn || uid == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
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

    state.uid = uid;
    state.state = UserConnectionState::Authenticated;
    state.last_active_ms = nowMs();
    uid_to_conn_[uid] = conn;
    return true;
}

uint64_t UserStateService::getUidByConn(
    const muduo::net::TcpConnectionPtr& conn) const {
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
    if (!conn) {
        return UserConnectionState::Connected;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conn_states_.find(conn.get());
    if (it == conn_states_.end()) {
        return UserConnectionState::Connected;
    }
    return it->second.state;
}

void UserStateService::updateHeartbeat(
    const muduo::net::TcpConnectionPtr& conn) {
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
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());
}
