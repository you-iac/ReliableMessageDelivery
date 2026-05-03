#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include <muduo/net/Buffer.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/base/Timestamp.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Message.pb.h"

struct ClientSession {
    uint64_t uid = 0;                       // 连接登录后绑定的用户 ID，0 表示未登录。
    muduo::net::TcpConnectionPtr conn;      // 当前用户对应的 TCP 连接。
    std::string recv_buffer;                // 每条连接独立维护 TCP 半包缓存。
};

// ChatServer 封装 Muduo TCP 服务、连接状态和在线消息转发逻辑。
// main 只需要创建对象并调用 start()，不需要知道具体回调如何注册。
class ChatServer {

public:
    using TcpConnectionPtr = muduo::net::TcpConnectionPtr;
    using Buffer = muduo::net::Buffer;
    using Timestamp = muduo::Timestamp;
    using Envelope  = message::Envelope;


    ChatServer(uint16_t port = 8080, int thread_num = 16);

    // 启动服务器并进入事件循环；正常情况下该函数会阻塞运行。
    bool start();

private:
    // Muduo 连接建立/断开回调：创建或清理连接级状态。
    void onConnection(  const TcpConnectionPtr& conn);

    // Muduo 消息回调：从字节流中解码 Envelope，再交给业务分发。
    void onMessage(     const TcpConnectionPtr& conn,
                        Buffer* buffer,
                        Timestamp);

    // 调用方必须已经持有 mutex_。
    ClientSession* findSessionLocked(const TcpConnectionPtr& conn);

    // 按 Envelope.type() 分发到具体业务处理函数。
    void dispatch(      const TcpConnectionPtr& conn,
                        const Envelope&         envelope);
    void handleLogin(   const TcpConnectionPtr& conn,
                        const Envelope&         envelope);
    void handleChat(    const TcpConnectionPtr& conn,
                        const Envelope&         envelope);
    void sendEnvelope(  const TcpConnectionPtr& conn,
                        const Envelope&         envelope);

    // Muduo 配了多个 IO 线程，连接表和在线表会被不同回调线程访问，因此统一加锁。
    std::mutex mutex_;
    std::unordered_map<const muduo::net::TcpConnection*, ClientSession> sessions_;
    std::unordered_map<uint64_t, TcpConnectionPtr> online_users_;

    // 第一版先用进程内自增 ID；后续持久化时再替换为可靠 msg_id 生成方案。
    std::atomic<uint64_t> next_msg_id_{1};

    uint16_t port_;
    int thread_num_;

};

#endif
