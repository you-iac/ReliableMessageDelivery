# 开发记录

记录项目设计过程、关键实现、踩坑问题和后续计划。

## 2026-05-04 CST

### 本阶段目标

将服务端从“`ChatServer` 直接处理业务”继续拆分为更清晰的事件驱动结构，并开始向可靠消息投递推进：

```text
ChatServer
  只负责 TCP 连接、半包缓存、Envelope 解码
        ↓
ServerEventDispatcher
  统一消费业务事件：Envelope / ConnectionClosed
        ↓
UserStateService
  维护在线用户状态
        ↓
MessageStore
  保存消息记录和投递状态
```

本阶段仍然是内存版实现，不接 MySQL / Redis。

### 已完成事项

1. 收敛 `ChatServer` 职责
   - `ChatServer` 不再处理 `LOGIN_REQ`、`CHAT_REQ`、`ACK` 等业务逻辑。
   - `ClientSession` 只保留连接级数据：

     ```cpp
     muduo::net::TcpConnectionPtr conn;
     std::string recv_buffer;
     ```

   - `ChatServer::onMessage()` 只负责：
     - 从 Muduo `Buffer` 读取二进制数据。
     - 追加到连接级 `recv_buffer`。
     - 调用 `MessageCodec::DecodeAll()` 解码完整 `Envelope`。
     - 将解码后的消息投递给 `ServerEventDispatcher`。

   - 连接断开时，`ChatServer` 只清理半包缓存，并投递 `ConnectionClosed` 事件。

2. 新增 `ServerEventDispatcher`
   - 引入统一业务事件队列：

     ```cpp
     struct ServerEvent {
         enum class Type {
             Envelope,
             ConnectionClosed
         };

         Type type;
         muduo::net::TcpConnectionPtr conn;
         message::Envelope envelope;
     };
     ```

   - 对外提供两个入队接口：

     ```cpp
     enqueueEnvelope(conn, envelope)
     enqueueConnectionClosed(conn)
     ```

   - 当前采用单队列 + 单 worker 线程，保证同一连接上的事件按入队顺序处理。
   - `handle()` 根据事件类型和 `Envelope.type()` 分发：
     - `LOGIN_REQ`
     - `CHAT_REQ`
     - `ACK`
     - `HEARTBEAT`
     - `ConnectionClosed`

3. 新增 `UserStateService`
   - 最终选择直接版在线状态表，不再引入 `ConnectionId / ConnectionRegistry`。
   - 当前维护两张表：

     ```cpp
     conn -> UserState
     uid  -> TcpConnectionPtr
     ```

   - 支持：
     - `bindUser(conn, uid)`
     - `removeConnection(conn)`
     - `getUidByConn(conn)`
     - `getConnByUid(uid)`
     - `updateHeartbeat(conn)`

   - 当前是单端在线策略：同一个 uid 后登录的连接会覆盖旧连接。

4. 接入登录、在线转发和连接清理
   - `LOGIN_REQ`：
     - 校验 `login_req` payload。
     - 校验 uid 非 0。
     - 调用 `UserStateService::bindUser()`。
     - 返回 `LOGIN_RESP`。

   - `CHAT_REQ`：
     - 校验 `chat_req` payload。
     - 通过 conn 查询当前登录 uid。
     - 校验 `from_uid` 和连接绑定 uid 一致。
     - 通过 `to_uid` 查询目标连接。
     - 在线时发送 `CHAT_PUSH`。
     - 给发送方返回 `ACK`。

   - `ConnectionClosed`：
     - 调用 `UserStateService::removeConnection(conn)` 清理在线状态。

   - `HEARTBEAT`：
     - 当前只刷新连接活跃时间。

5. 新增 `MessageStore`
   - 引入内存版消息账本：

     ```cpp
     enum class MessageStatus {
         Pending,
         Delivered,
         Acked,
         Failed
     };

     struct MessageRecord {
         uint64_t msg_id;
         uint64_t from_uid;
         uint64_t to_uid;
         std::string content;
         std::string client_msg_id;
         MessageStatus status;
         uint64_t created_at_ms;
         uint64_t delivered_at_ms;
         uint64_t acked_at_ms;
         int retry_count;
     };
     ```

   - `MessageStore` 自己生成 `msg_id`，外部只传业务字段。
   - 已实现：
     - `createMessage()`
     - `markDelivered()`
     - `markAcked()`
     - `markFailed()`
     - `getMessage()`
     - `getPendingMessages(to_uid)`

   - `handleChat()` 现在先创建 `MessageRecord`：
     - 如果目标在线，发送 `CHAT_PUSH` 并 `markDelivered()`。
     - 如果目标离线，消息保持 `Pending`，等待后续离线补发能力。

6. 重构客户端压测入口
   - `Client` 新增 `setVerbose(bool)`，默认关闭每条消息的发送/接收日志。
   - `main_client.cpp` 改成多线程压测程序：

     ```bash
     ./bin/client [sender_count] [messages_per_sender] [verbose] [wait_timeout_seconds]
     ```

   - 每个发送线程使用独立 `Client` 连接，避免多个线程同时写同一个 socket。
   - 不再固定 `sleep 3s` 截断统计，而是等待：
     - `sender_acks == total_messages`
     - `receiver_chat_pushes == total_messages`
     - 或者超时。

   - 分开统计：
     - 客户端写入吞吐。
     - 服务端 ACK 完成吞吐。
     - 接收方 PUSH 完成吞吐。

### 设计记录

#### 为什么把连接断开也做成事件

登录请求是通过 `Envelope` 进入业务队列的，如果连接断开直接调用用户状态清理函数，可能出现顺序竞态：

```text
LOGIN_REQ 入队
连接断开，直接清理
worker 后处理 LOGIN_REQ
已经断开的 conn 又被标记为在线
```

因此连接断开也进入 `ServerEventDispatcher` 的同一个队列：

```text
LOGIN_REQ
CHAT_REQ
ConnectionClosed
```

当前单 worker 下，事件会按入队顺序处理，状态更容易推理。

#### 为什么没有使用单例 UserManager

讨论过把在线用户表设计成单例，优点是“所有地方都能访问”。但最终当前实现采用普通成员对象：

```cpp
ServerEventDispatcher {
    UserStateService user_state_;
};
```

这样依赖关系更明确，也方便后续测试和替换。

#### 为什么放弃 ConnectionId / ConnectionRegistry

曾经设计过：

```text
UserStateService:
  uid <-> ConnectionId

ConnectionRegistry:
  ConnectionId <-> TcpConnectionPtr
```

这个设计更解耦，但对当前第一版在线转发和可靠投递原型来说过重。最终回到直接版：

```text
conn -> uid
uid  -> conn
```

这样代码更短，能更快推进核心可靠投递功能。

#### 为什么 MessageStore 不直接接收 Envelope

`MessageStore` 只负责存储消息记录，不理解协议对象：

```cpp
createMessage(from_uid, to_uid, content, client_msg_id)
```

`ServerEventDispatcher` 负责解析 `Envelope`、校验登录态和字段合法性。这样可以避免 `MessageStore` 同时承担协议解析、业务校验和存储三种职责。

#### 为什么 msg_id 由 MessageStore 生成

`msg_id` 是服务端消息账本 ID，应该由创建消息记录的模块统一生成。

这样以后从内存版迁移到 MySQL 时，可以把自增 ID、雪花 ID 或数据库 ID 生成逻辑封装在 `MessageStore` 内部，对业务层保持接口稳定。

#### 为什么暂时没有单独投递线程

当前 `handleChat()` 同步完成：

```text
createMessage
查目标是否在线
在线则投递并 markDelivered
离线则保持 Pending
```

投递线程、重试线程和离线补发线程会在 ACK 状态机和 Pending 查询流程稳定后再引入。

### 踩坑和取舍

#### 1. 多 worker 不能直接抢同一个队列

如果未来把 worker 改成多个线程直接抢同一个队列，会出现：

```text
LOGIN_REQ 先入队
CHAT_REQ 后入队
worker2 先处理 CHAT_REQ
```

当前为了保证顺序，保持单 worker。后续如果需要并发，应采用固定数量 shard 队列：

```text
hash(conn) % shard_count
```

同一连接固定进入同一个 shard，不同连接并行处理。

#### 2. 压测不能只看 send() 成功

最初压测统计的是客户端写入 socket 成功数量，但这只说明数据进入本机内核发送缓冲区，并不代表：

```text
服务端已处理
发送方已收到 ACK
接收方已收到 PUSH
```

因此压测改成分别统计：

```text
client_write_throughput
server_ack_throughput
receiver_push_throughput
```

#### 3. 每条消息打印日志会严重影响压测

客户端原来每次发送和接收都会打印完整 `Envelope`。高并发压测时，`std::cout` 会成为明显瓶颈。

现在通过 `setVerbose(false)` 默认关闭逐条日志。

### 运行结果测试

构建通过：

```bash
cmake --build build
```

压测命令：

```bash
./bin/client 32 10000 quiet 60
```

结果：

```text
stress config: senders=32, messages_per_sender=10000, total_messages=320000, verbose=false, wait_timeout_seconds=60

Stress summary
  send_elapsed_seconds:  0.084
  ack_elapsed_seconds:   37.111
  push_elapsed_seconds:  37.111
  total_elapsed_seconds: 37.111
  send_ok:               320000
  send_failed:           0
  receiver_login_resps:  1
  receiver_chat_pushes:  320000
  sender_login_resps:    32
  sender_acks:           320000
  client_write_throughput: 3.80952e+06 msg/s
  server_ack_throughput:   8622.78 msg/s
  receiver_push_throughput: 8622.78 msg/s
```

说明：

```text
客户端写入吞吐很高，但服务端当前单 worker 的完整处理吞吐约 8.6k msg/s。
这符合当前“单业务线程 + 内存状态表 + Protobuf 编解码 + 双向回包”的阶段性预期。
```

另一次 16 sender、10 秒等待的结果显示超时前未处理完所有消息：

```text
receiver_chat_pushes: 65588 / 160000
sender_acks:          62052 / 160000
```

说明固定短超时下不能代表完整吞吐，必须结合 ACK/PUSH 完成量判断。

### 当前能力边界

目前已经具备：

```text
TCP 长连接
Protobuf 编解码
业务事件队列
登录绑定
在线用户表
在线消息转发
发送方 ACK
连接断开清理
心跳活跃时间更新
内存版 MessageStore
Pending / Delivered / Acked / Failed 状态结构
多线程客户端压测
```

还没有实现：

```text
接收方 ACK 状态流转
离线消息上线补发
未 ACK 超时重试
client_msg_id 幂等去重
消息持久化到 MySQL
心跳超时扫描
metrics 指标接口
多 worker shard 队列
```

### 后续计划

1. 接收方 ACK 处理
   - 客户端收到 `CHAT_PUSH` 后返回 `ACK`。
   - 服务端根据 `msg_id` 调用 `MessageStore::markAcked()`。

2. 离线消息补发
   - 用户登录成功后调用 `getPendingMessages(uid)`。
   - 对 Pending 消息逐条发送 `CHAT_PUSH`。
   - 发送成功后 `markDelivered()`。

3. 重试机制
   - 定时扫描 `Delivered` 但长时间未 `Acked` 的消息。
   - 重投并增加 `retry_count`。
   - 超过最大次数后标记 `Failed`。

4. 幂等去重
   - 使用 `from_uid + client_msg_id -> msg_id`。
   - 客户端重发时返回已有 `msg_id`，不重复创建消息。

5. 压测优化
   - 关闭服务端逐条 `LOG_INFO`。
   - 区分服务端处理耗时、客户端接收耗时和 socket 缓冲积压。
   - 后续尝试 shard worker 模型提升并发处理能力。
