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
    void addConnection(const muduo::net::TcpConnectionPtr& conn);
    void removeConnection(const muduo::net::TcpConnectionPtr& conn);
    
    bool bindUser(const muduo::net::TcpConnectionPtr& conn, uint64_t uid);

    uint64_t getUidByConn(const muduo::net::TcpConnectionPtr& conn) const;
    muduo::net::TcpConnectionPtr getConnByUid(uint64_t uid) const;

    bool isAuthenticated(const muduo::net::TcpConnectionPtr& conn) const;
    UserConnectionState getState(const muduo::net::TcpConnectionPtr& conn) const;

    void updateHeartbeat(const muduo::net::TcpConnectionPtr& conn);

private:
    static uint64_t nowMs();

    mutable std::mutex mutex_;

    // TcpConnection* -> UserState，用于从连接反查当前登录用户。
    std::unordered_map<const muduo::net::TcpConnection*, UserState> conn_states_;

    // uid -> TcpConnectionPtr，用于消息投递时快速找到接收方在线连接。
    std::unordered_map<uint64_t, muduo::net::TcpConnectionPtr> uid_to_conn_;
};

#endif
