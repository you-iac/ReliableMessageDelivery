
#ifndef SERVER_EVENT_DISPATCHER_H
#define SERVER_EVENT_DISPATCHER_H

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include "Message.pb.h"
#include "MessageStore.h"
#include "UserStateService.h"
#include "ShardedThreadPool.h"

#include <muduo/net/TcpConnection.h>


// ServerEventDispatcher 是服务端业务事件分发器。
//
// 当前它主要消费 ChatServer 投递过来的 Envelope 事件；后续也可以扩展为消费
// ConnectionClosed、HeartbeatTimeout 等非 Envelope 事件，让 ChatServer 保持网络层职责。
//
// ServerEventDispatcher 负责：
//   1. 接收 ChatServer 投递过来的业务事件。
//   2. 按连接分片提交到业务线程池。
//   3. 根据 Envelope.type() 分发到登录、聊天、ACK 等业务处理函数。
class ServerEventDispatcher {
public:
    ServerEventDispatcher();
    ~ServerEventDispatcher();

    // 启动后台业务线程池。重复调用是安全的。
    void start();   

    // 停止后台业务线程池。
    // stop() 会阻止新任务入队，并等待已经提交的任务处理完成后退出。
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

    // 返回业务线程池每个 worker 的待处理任务数快照。
    std::vector<std::size_t> getWorkerQueueSizes() const;
private:
    // 队列中的一条业务事件。
    //
    // Envelope 表示来自客户端的 protobuf 消息。
    // ConnectionClosed 表示 TCP 连接已断开。
    // 两类事件按连接分片提交到线程池，尽量保持同一连接上的事件顺序。
    struct ServerEvent {
        enum class Type {
            Envelope,
            ConnectionClosed
        };

        Type type = Type::Envelope;
        muduo::net::TcpConnectionPtr conn;
        message::Envelope envelope;
    };

    void deliveryLoop();// 后台投递循环：扫描 Delivered 超时消息并重投。
    
    void handle     (const ServerEvent& event);// 统一业务分发入口，根据事件类型或 Envelope.type() 调用具体处理函数。
    void handleLogin(const ServerEvent& event);// 处理 LOGIN_REQ。后续会接入 SessionService 完成 uid 和连接绑定
    void handleChat (const ServerEvent& event);// 处理 CHAT_REQ。后续会接入在线查询、离线存储、ACK 和重试逻辑。
    void handleAck  (const ServerEvent& event);// 处理客户端 ACK。后续会接入消息状态机和未 ACK 队列。
    void handleHeartbeat(const ServerEvent& event);// 处理心跳消息。后续会更新连接活跃时间，并定期清理长时间未心跳的连接。
    void handleConnectionClosed(const ServerEvent& event);// 处理连接断开事件。后续会清理用户在线状态。
    
    void deliverLoginMessages(uint64_t uid);// 用户上线后优先补发 Pending；没有 Pending 时回放最近消息。
    bool deliverPendingMessages(uint64_t uid);// 用户上线后补发该用户的 Pending 消息。
    void sendRecentMessages(uint64_t uid, std::size_t limit);// 给登录用户发送最近消息，不改变消息投递状态。
    bool tryDeliverMessage(const MessageRecord& record);// 普通投递，目标离线时保持 Pending。
    bool retryDeliverMessage(const MessageRecord& record);// 重试投递，发送前递增重试次数。
    void sendEnvelope(const muduo::net::TcpConnectionPtr& conn,
                      const message::Envelope& envelope);
    void sendErrorAck(const muduo::net::TcpConnectionPtr& conn,
                      uint64_t seq,
                      const std::string& reason);

    
    std::mutex mutex_;// 保护 stopped_ 和 worker 生命周期状态。
    bool stopped_ = true;// true 表示 dispatcher 未运行或正在停止。

    ShardedThreadPool pool_; // 业务线程池；同一连接按 conn 指针分片，尽量保持连接内事件顺序。
    std::thread delivery_worker_; // 后台投递线程，负责超时重投。
    UserStateService user_state_;
    MessageStore message_store_;
};



#endif
