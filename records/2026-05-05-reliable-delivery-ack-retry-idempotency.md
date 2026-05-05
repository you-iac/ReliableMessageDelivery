# 开发记录

记录项目设计过程、关键实现、踩坑问题和后续计划。

## 2026-05-05 CST

### 本阶段目标

把项目从“在线转发 + 内存消息账本”继续推进到第一版可靠投递闭环：

```text
客户端发送 CHAT_REQ
        ↓
服务端按 client_msg_id 幂等创建消息
        ↓
目标在线：投递 CHAT_PUSH
目标离线：消息保持 Pending
        ↓
接收方收到 CHAT_PUSH 后回 ACK
        ↓
服务端 markAcked
        ↓
Delivered 超时未 ACK 时后台重投
```

本阶段仍然是内存版实现，不接 MySQL / Redis。

### 已完成事项

1. 客户端登录确认语义收紧
   - `startClient()` 不再只表示 `LOGIN_REQ` 写入 socket 成功。
   - 现在流程是：

     ```text
     connect()
     启动 receiverLoop()
     发送 LOGIN_REQ
     等待 LOGIN_RESP
     LOGIN_RESP.ok == true 后 startClient() 才返回 true
     ```

   - 新增登录等待状态：

     ```cpp
     std::mutex login_mutex;
     std::condition_variable login_cv;
     bool login_done;
     bool login_ok;
     std::string login_reason;
     ```

   - `sendMessage()` 会先检查登录状态，未登录成功时直接返回失败。

2. 客户端接收分发
   - `receiverLoop()` 只负责阻塞 `recv()`、拼接 `recv_buffer`、解码并交给 `handleEnvelope()`。
   - `handleEnvelope()` 按类型分发：

     ```text
     LOGIN_RESP -> handleLoginResponse()
     ACK        -> handleAck()
     CHAT_PUSH  -> handleChatPush()
     ```

   - `message_queue` 只保存真正业务推送，也就是 `CHAT_PUSH`。

3. 客户端发送侧 pending ACK 和重试
   - 新增 `send_mutex`，保护一个完整 frame 的发送过程。
   - 新增 `pending_acks`，记录已发送但还没收到服务端 ACK 的请求：

     ```cpp
     struct PendingAck {
         message::Envelope envelope;
         uint64_t last_send_ms;
         int retry_count;
     };
     ```

   - 登录成功后启动 `retryLoop()`，超时未 ACK 时重发原始 `Envelope`。
   - 重发不会重新生成 `client_msg_id`。

4. client_msg_id 生成调整
   - 原来使用 `uid + seq`，客户端重启或 seq 重置后有误判风险。
   - 当前改为：

     ```text
     uid + timestamp_ms + local_counter
     ```

   - `seq` 用于请求和 ACK 对应，`client_msg_id` 用于服务端幂等去重。

5. 接收方 ACK 回包
   - 客户端收到 `CHAT_PUSH` 后：

     ```text
     校验 chat_push payload
     chat_push_count++
     pushMessage(envelope)
     发送 ACK(msg_id)
     ```

6. 服务端幂等去重
   - `MessageStore` 新增去重索引：

     ```cpp
     std::unordered_map<std::string, uint64_t> client_msg_index_;
     ```

   - key 为：

     ```text
     from_uid + ":" + client_msg_id
     ```

   - `createMessage()` 返回 `CreateMessageResult`：

     ```cpp
     struct CreateMessageResult {
         MessageRecord record;
         bool created;
     };
     ```

   - 如果命中重复请求，不重复创建和投递，只返回原 `msg_id` 的 ACK。

7. 服务端接收方 ACK 状态流转
   - `handleAck()` 现在会校验 ACK payload、`msg_id`、当前连接 uid。
   - 只有 ACK 连接的登录 uid 等于消息 `to_uid`，才允许更新状态。
   - `ack.ok == true` 时 `markAcked(msg_id)`。
   - `ack.ok == false` 时 `markFailed(msg_id)`。

8. 服务端离线补发和超时重投
   - `MessageStore` 新增：

     ```cpp
     markPending(msg_id)
     incrementRetryCount(msg_id)
     getTimeoutDeliveredMessages(now_ms, timeout_ms)
     ```

   - `ServerEventDispatcher` 新增后台 `delivery_worker_`。
   - 登录成功后调用 `deliverPendingMessages(uid)`。
   - 新消息、离线补发和超时重投统一走 `tryDeliverMessage(msg_id, is_retry)`。

### 设计记录

#### 为什么需要服务端幂等去重

客户端发送消息后，如果服务端已经保存并返回 ACK，但 ACK 在网络中丢失，客户端会超时重发。

如果服务端不去重，会出现：

```text
第一次 CHAT_REQ -> msg_id=1
重发 CHAT_REQ   -> msg_id=2
再次重发        -> msg_id=3
```

用户只发了一条业务消息，接收方却会看到多条重复消息。

因此服务端使用：

```text
from_uid + client_msg_id -> msg_id
```

同一业务消息重复到达时，返回第一次的 `msg_id`，不重复创建和投递。

#### 为什么客户端还需要 msg_id 去重

服务端幂等只能保证同一个 `client_msg_id` 不重复创建消息。

但服务端重投和接收方 ACK 之间存在正常竞态：

```text
deliveryLoop 准备重投 msg_id=1
接收方 ACK 到达
服务端 markAcked(1)
deliveryLoop 可能已经发出一次重复 CHAT_PUSH
```

服务端最终状态可以是正确的 `Acked`，但客户端仍可能重复收到同一个 `msg_id`。

因此后续客户端需要：

```text
第一次收到 msg_id：入队、计数、回 ACK
重复收到 msg_id：只回 ACK，不入队、不计数
```

#### 为什么现在吞吐不高但 CPU 不满

当前服务端主业务路径仍然是单 worker：

```text
ServerEventDispatcher::workerLoop()
```

所有 `LOGIN_REQ / CHAT_REQ / ACK / ConnectionClosed` 都进入同一个业务队列顺序处理。

同时压测模型是：

```text
多个 sender -> 一个 receiver
```

所有 `CHAT_PUSH` 最终集中写入一个接收方连接和一个客户端接收线程。

因此当前瓶颈更可能在单业务 worker、单接收方连接、日志输出、socket 缓冲、内存 map 和重试重复包，而不是 CPU 算力。

### 压测观察

小规模压测已经能完整闭环：

```text
./bin/client 16 200 quiet 60

total_messages:        3200
sender_acks:           3200
receiver_chat_pushes:  3200
```

中高压力下仍有重复推送：

```text
./bin/client 10 10000 quiet 60

total_messages:        100000
sender_acks:           100701
receiver_chat_pushes:  129052
```

结论：

```text
服务端幂等创建已经明显减少重复创建。
接收端还没有按 msg_id 去重，所以 receiver_chat_pushes 会包含重复 PUSH。
当前 receiver_push_throughput 不能代表唯一业务消息吞吐。
```

### 当前能力边界

目前已经具备：

```text
TCP 长连接
Protobuf 编解码
登录确认
在线用户表
在线消息投递
离线 Pending
用户上线补发 Pending
发送方 ACK
接收方 ACK
服务端 markAcked
服务端超时重投
客户端发送重试
client_msg_id 幂等去重
MessageStore 内存状态机
```

还没有实现：

```text
客户端 msg_id 去重
Acked / Failed 消息清理
严格按创建时间顺序补发
心跳超时扫描
MySQL / Redis 持久化
metrics 指标接口
多 worker shard 队列
inflight 限流
关闭或降级逐条服务端日志
```

### 后续计划

1. 客户端按 `msg_id` 去重
   - 收到重复 `CHAT_PUSH(msg_id)` 时只回 ACK，不重复入队。
   - 统计中区分 `push_packets` 和 `unique_push_messages`。

2. 调整重试策略
   - 当前压测中固定超时容易误判积压为丢包。
   - 需要调大超时、支持压测模式关闭重试，或增加 inflight 限流。

3. 清理内存消息
   - 当前 `MessageStore` 不清理 `Acked` 消息。
   - 大压测时内存会持续增长。
   - 后续可先做内存版保留窗口，再迁移到 MySQL。

4. 补离线补发专项测试
   - 构造接收方离线、发送方发消息、接收方上线的流程。
   - 验证 Pending -> Delivered -> Acked。

5. 优化服务端并发模型
   - 当前单 worker 简单但吞吐有限。
   - 后续可考虑按 uid 或 conn hash 做 shard worker。
