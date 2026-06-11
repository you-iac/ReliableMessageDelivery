
#ifndef SERVER_EVENT_DISPATCHER_H
#define SERVER_EVENT_DISPATCHER_H

#include <condition_variable>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

#include "Message.pb.h"
#include "MessageStore.h"
#include "UserStateService.h"

#include <muduo/net/TcpConnection.h>


// ServerEventDispatcher 是服务端业务事件分发器。
//
// 当前它主要消费 ChatServer 投递过来的 Envelope 事件；后续也可以扩展为消费
// ConnectionClosed、HeartbeatTimeout 等非 Envelope 事件，让 ChatServer 保持网络层职责。
//
// ServerEventDispatcher 负责：
//   1. 维护线程安全的待处理队列。
//   2. 在 worker 线程中按顺序取出请求。
//   3. 根据 Envelope.type() 分发到登录、聊天、ACK 等业务处理函数。
class ServerEventDispatcher {
public:
    ServerEventDispatcher();
    ~ServerEventDispatcher();

    // 启动后台 worker 线程。重复调用是安全的。
    void start();   

    // 停止后台 worker 线程。
    // stop() 会唤醒 worker，并等待已经入队的任务处理完成后退出。
    // 重复调用是安全的。
    void stop();

    // 将一条已解码的 Envelope 投递到业务队列。
    //
    // conn 表示该消息来自哪条 TCP 连接，后续处理登录响应、ACK 或推送失败
    // 时需要通过 conn 回包。Envelope 只表示业务内容，不能单独表达连接上下文。
    bool enqueueEnvelope(const muduo::net::TcpConnectionPtr& conn,
                         const message::Envelope& envelope);

    // 将连接断开事件投递到业务队列。
    //
    // 连接断开虽然不是 protobuf Envelope，但它会影响用户在线状态，
    // 所以也应该进入同一个业务事件队列，和 LOGIN_REQ 等事件保持顺序处理。
    bool enqueueConnectionClosed(const muduo::net::TcpConnectionPtr& conn);
private:
    // 队列中的一条业务事件。
    //
    // Envelope 表示来自客户端的 protobuf 消息。
    // ConnectionClosed 表示 TCP 连接已断开。
    // 两类事件共用一个队列，保证同一条连接上的登录、消息、断开按入队顺序处理。
    struct ServerEvent {
        enum class Type {
            Envelope,
            ConnectionClosed
        };

        Type type = Type::Envelope;
        muduo::net::TcpConnectionPtr conn;
        message::Envelope envelope;
    };

    // 所有从 ChatServer 投递过来的待处理请求。
    // 该队列只允许在 mutex_ 保护下访问。
    std::queue<ServerEvent> inbox_;
    
    void workerLoop();// worker 主循环：等待队列非空，取出请求，然后在锁外处理业务。
    void deliveryLoop();// 后台投递循环：扫描 Delivered 超时消息并重投。
    
    void handle     (const ServerEvent& event);// 统一业务分发入口，根据事件类型或 Envelope.type() 调用具体处理函数。
    void handleLogin(const ServerEvent& event);// 处理 LOGIN_REQ。后续会接入 SessionService 完成 uid 和连接绑定
    void handleChat (const ServerEvent& event);// 处理 CHAT_REQ。后续会接入在线查询、离线存储、ACK 和重试逻辑。
    void handleAck  (const ServerEvent& event);// 处理客户端 ACK。后续会接入消息状态机和未 ACK 队列。
    void handleHeartbeat(const ServerEvent& event);// 处理心跳消息。后续会更新连接活跃时间，并定期清理长时间未心跳的连接。
    void handleConnectionClosed(const ServerEvent& event);// 处理连接断开事件。后续会清理用户在线状态。
    
    void deliverPendingMessages(uint64_t uid);// 用户上线后补发该用户的 Pending 消息。
    bool tryDeliverMessage(const MessageRecord& record);// 普通投递，目标离线时保持 Pending。
    bool retryDeliverMessage(const MessageRecord& record);// 重试投递，发送前递增重试次数。
    void sendEnvelope(const muduo::net::TcpConnectionPtr& conn,
                      const message::Envelope& envelope);
    void sendErrorAck(const muduo::net::TcpConnectionPtr& conn,
                      uint64_t seq,
                      const std::string& reason);

    
    std::mutex mutex_;// 保护 inbox_、stopped_ 和 worker 生命周期状态。
    std::condition_variable not_empty_;// 当队列为空时 worker 阻塞等待；入队接口或 stop() 会唤醒它。
    bool stopped_ = true;// true 表示 dispatcher 未运行或正在停止。
    
    std::thread worker_;    // 当前第一版的单 worker 线程。
    std::thread delivery_worker_; // 后台投递线程，负责超时重投。
    UserStateService user_state_;
    MessageStore message_store_;
};



#endif
