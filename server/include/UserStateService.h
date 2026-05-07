#ifndef USER_STATE_SERVICE_H
#define USER_STATE_SERVICE_H

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <muduo/net/TcpConnection.h>

// UserConnectionState 表示一条连接在用户状态表中的业务状态。
enum class UserConnectionState {
    Connected,
    Authenticated
};

struct UserState {
    uint64_t uid = 0;
    muduo::net::TcpConnectionPtr conn;
    UserConnectionState state = UserConnectionState::Connected;

    // 最近一次活跃时间。后续 HEARTBEAT 到达时刷新，用于超时清理。
    uint64_t last_active_ms = 0;
};

// UserStateService 维护用户在线状态和连接到用户的绑定关系。
//
// 职责：
//   1. conn -> uid，用于通过连接查询登录用户。
//   2. uid -> conn，用于通过用户查询当前在线连接。
//   3. 记录最近活跃时间，后续可用于心跳超时清理。
class UserStateService {
public:
    // 新连接建立后登记连接状态。初始状态为 Connected，尚未绑定 uid。
    void addConnection(const muduo::net::TcpConnectionPtr& conn);

    // 连接断开时移除状态，并在必要时清理该连接对应的 uid 在线索引。
    void removeConnection(const muduo::net::TcpConnectionPtr& conn);

    // 将连接绑定到指定用户。uid 必须非 0；绑定成功后连接进入 Authenticated 状态。
    // 同一个 uid 只保留最新连接，旧连接会被降回 Connected 状态。
    bool bindUser(const muduo::net::TcpConnectionPtr& conn, uint64_t uid);

    // 根据连接查询已认证用户 uid。未认证、未登记或空连接返回 0。
    uint64_t getUidByConn(const muduo::net::TcpConnectionPtr& conn) const;

    // 根据 uid 查询当前在线连接。用户不在线时返回空 TcpConnectionPtr。
    muduo::net::TcpConnectionPtr getConnByUid(uint64_t uid) const;

    // 判断连接是否已完成用户绑定。
    bool isAuthenticated(const muduo::net::TcpConnectionPtr& conn) const;

    // 查询连接当前业务状态。未知连接按 Connected 处理。
    UserConnectionState getState(const muduo::net::TcpConnectionPtr& conn) const;

    // 收到心跳后刷新连接最近活跃时间，不改变认证状态。
    void updateHeartbeat(const muduo::net::TcpConnectionPtr& conn);

private:
    // 返回当前系统时间的毫秒时间戳。
    static uint64_t nowMs();

    mutable std::mutex mutex_;

    // TcpConnection* -> UserState，用于从连接反查当前登录用户。
    std::unordered_map<const muduo::net::TcpConnection*, UserState> conn_states_;

    // uid -> TcpConnectionPtr，用于消息投递时快速找到接收方在线连接。
    std::unordered_map<uint64_t, muduo::net::TcpConnectionPtr> uid_to_conn_;
};

#endif
