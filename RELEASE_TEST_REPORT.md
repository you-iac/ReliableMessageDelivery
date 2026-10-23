# 候选版本测试报告

- 日期：2026-10-21
- 分支：`release/0.1.0`
- 候选提交：`7fcb7b9`
- `develop` 基准提交：`805834a`
- 环境：Linux 上的 Docker Compose

## 自动化测试结果

| 检查项 | 结果 | 验证依据 |
| --- | --- | --- |
| Compose 配置 | 通过 | `docker compose config --quiet` |
| 补丁格式 | 通过 | `git diff --check` |
| C++ 构建 | 通过 | 在 Docker 中成功构建 Server 和 Client 的 Release 目标 |
| C++ 协议测试 | 通过 | CTest `codec_test`；覆盖完整帧、半包、粘包、尾部半包和无效帧 |
| Gateway TypeScript 构建 | 通过 | `npm test` 期间执行 `tsc -p tsconfig.json` |
| Gateway 认证测试 | 通过 | 4/4 项 Node 测试通过 |
| 首轮 HTTP/WebSocket 冒烟测试 | 通过 | 注册、登录、token 认证会话、消息推送、发送方 ACK、接收方 ACK；`msg_id=1` |
| Redis 消息状态 | 通过 | `rmd:msg:1` 到达 `Acked` 状态；`retry_count=0` |
| IMServer 重启恢复 | 通过 | 再次运行端到端冒烟测试并成功；`msg_id=2` |
| Redis 重启恢复 | 通过 | 再次运行端到端冒烟测试并成功；`msg_id=3` |

## 已知限制与后续事项

- 认证用户和 token 保存在 Gateway 进程内存中。重启 Gateway 会丢失这些数据；
  持久化认证不在本次演示版本的范围内。
- `npm audit --omit=dev` 报告 `protobufjs 8.6.4` 存在一个中危直接依赖问题
  （GHSA-jfj6-75fj-8934 和 GHSA-j3f2-48v5-ccww）。当前应用加载的是受控的
  本地 `.proto` 文件，而不是用户提供的 schema 文本，但在生产发布前仍应升级
  该依赖并重新运行整套测试。
- 添加 `v0.1.0` 标签之前，仍需按照 `TESTING.md` 完成基于浏览器的离线消息
  投递和重复显示行为人工检查。
- Redis 报告宿主机未启用 `vm.overcommit_memory`。这没有阻塞本轮测试，但生产
  环境应在发布前将其设置为 `1` 并持久化该配置。

## 发布结论

现有自动化检查均已通过。在完成剩余的人工检查和依赖处理决策之前，
该提交适合作为初始候选版本，但还不能作为最终的 `v0.1.0` 发布。
