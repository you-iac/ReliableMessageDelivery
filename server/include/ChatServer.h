#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include <muduo/net/Buffer.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/base/Timestamp.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Message.pb.h"
#include "ServerEventDispatcher.h"

// 连接级状态。当前只保存 TCP 连接和半包缓存，业务状态交给后续服务处理。
// ChatServer 封装 Muduo TCP 服务、连接状态和在线消息转发逻辑。
// 收取二进制数据，解码成 protobuf Envelope，
//根据 Envelope.type() 分发到不同的业务处理函数，最后再编码成二进制数据回包。
struct ClientSession {
    muduo::net::TcpConnectionPtr conn;
    std::string recv_buffer;
};
class ChatServer {

public:
    using TcpConnectionPtr = muduo::net::TcpConnectionPtr;
    using Buffer = muduo::net::Buffer;
    using Timestamp = muduo::Timestamp;
    using Envelope  = message::Envelope;


    ChatServer(uint16_t port = 8080, int thread_num = 16);

    /// 启动服务器并进入事件循环；正常情况下该函数会阻塞运行。
    bool start();

private:
    /// Muduo 连接建立/断开回调：创建或清理连接级状态。
    void onConnection(  const TcpConnectionPtr& conn);

    /// Muduo 消息回调：从字节流中解码 Envelope，再交给业务分发。
    void onMessage(     const TcpConnectionPtr& conn,
                        Buffer* buffer,
                        Timestamp);

    /// 调用方必须已经持有 mutex_。
    ClientSession* findSessionLocked(const TcpConnectionPtr& conn);

    uint16_t port_;
    int thread_num_;
    std::mutex mutex_;
    std::unordered_map<const muduo::net::TcpConnection*, ClientSession> sessionsTable_;

    ServerEventDispatcher dispatcher_; // 业务事件分发器，负责处理 ChatServer 投递的事件。
};

#endif
