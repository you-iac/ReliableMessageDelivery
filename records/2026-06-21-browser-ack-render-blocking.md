# 2026-06-21 浏览器接收链路 ACK 被渲染阻塞记录

## 现象

压力测试时，10 个发送用户各发送 10000 条消息给同一个浏览器用户。Redis 中按 `from_uid -> to_uid=1` 统计，每个发送用户确实只有 10000 条唯一消息；但浏览器会话列表中的未读数字可能显示为 25000 到 30000 左右。

这说明浏览器侧显示的数字不是去重后的唯一消息数，而是当前前端累计收到的推送次数。

### 根因

当前浏览器收到 `type=message` 后的流程是：

```text
WebSocket 收到 JSON
  -> JSON.parse
  -> appendIncomingMessage()
     -> 写入 conversation.messages
     -> 更新 lastMessage / unread
     -> renderConversationList()
     -> renderMessages()
  -> sendMessageAck(msg_id)
```

ACK 发送发生在前端状态更新和 DOM 渲染之后。压力测试时浏览器主线程会被 JSON 处理、数组增长、会话列表重绘、消息窗口重绘阻塞，导致 `message_ack` 不能及时发回 Gateway/IMServer。

IMServer 的投递语义是 at-least-once：服务端推送 `CHAT_PUSH` 后，如果超过 ACK 超时时间仍未收到接收方 ACK，就会重投同一个 `msg_id`。当浏览器 ACK 被渲染拖慢时，就会形成：

```text
消息多
  -> 浏览器渲染慢
  -> ACK 慢
  -> IMServer 认为未确认并重投
  -> 浏览器收到重复 msg_id
  -> 前端再次 push / unread +1
  -> 渲染更慢
```

### 当前问题点

1. 浏览器端没有按 `msg_id` 对接收消息做幂等去重。
2. 重复 `CHAT_PUSH` 会被当成新消息插入 `conversation.messages`。
3. 会话列表的未读数字表示收到的推送次数，不表示唯一消息条数。
4. 浏览器 ACK 不够早，会被 UI 渲染阻塞。

### 后续修复方向

浏览器接收消息时应该改为：

```text
收到 message
  -> 先按 msg_id 判断是否重复
  -> 立即 sendMessageAck(msg_id)
  -> 如果是重复消息，不重复显示，但仍然 ACK
  -> 如果是新消息，放入待渲染队列
  -> 批量刷新 conversationList / messageList
```

关键原则：

1. ACK 优先级高于 UI 渲染。
2. 接收方必须基于 `msg_id` 幂等去重。
3. 重复消息仍然要 ACK，避免服务端继续重投。
4. UI 渲染要批处理，避免每条消息都触发完整重绘。

可观测性建议：

1. Gateway 记录 IMServer `CHAT_PUSH` 接收速率、Browser `ws.send` 速率和 `ws.bufferedAmount`。
2. 浏览器使用 DevTools Performance 查看主线程 long task。
3. 前端分别统计 `received_push_count`、`unique_msg_count`、`duplicate_push_count` 和 `ack_sent_count`。
