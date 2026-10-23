# 测试与发布流程

长期维护的分支是 `develop` 和 `main`。从 `develop` 创建候选发布分支，
在不增加新功能的前提下进行稳定性修复。所有必要检查通过后，将候选
发布分支分别合并到 `main` 和 `develop`。

```bash
git switch develop
git pull --ff-only origin develop
git switch -c release/0.1.0
```

## 自动化检查

运行 Gateway 单元测试：

```bash
cd web_gateway
npm ci
npm test
```

构建 Server 镜像时，由 CTest 运行 C++ 协议测试：

```bash
docker compose build server gateway
```

启动候选版本，并运行 HTTP/WebSocket 端到端冒烟测试：

```bash
docker compose up -d --no-build --wait --wait-timeout 60
cd web_gateway
npm run test:e2e
```

如果宿主机没有安装 Node.js，可以构建并使用 Gateway 的构建阶段作为测试运行环境：

```bash
docker build --target build -f web_gateway/Dockerfile -t rmd-web-gateway:test .
docker run --rm --network host \
  -e RMD_BASE_URL=http://127.0.0.1:6780 \
  rmd-web-gateway:test npm run test:e2e
```

冒烟测试会验证首页、注册、登录、基于 token 认证的 WebSocket 会话、
发送方 ACK、接收方消息推送以及接收方消息 ACK。

## 人工发布检查

- Redis 状态健康，Server 和 Gateway 持续运行且没有发生重启。
- 两个浏览器用户可以双向收发消息。
- 向离线用户发送的消息能在该用户登录后送达。
- 已投递并确认的消息不会重复显示。
- 重启 Server 和 Redis 后，已存储的消息仍然保留，服务能够恢复。
- 已记录 Gateway 重启行为：认证信息目前保存在内存中，这是演示版本的已知限制。
- 日志中没有崩溃、反复连接失败或无效帧。

所有检查通过后，将 `release/0.1.0` 合并到 `main`，为合并提交添加
`v0.1.0` 标签，然后将发布分支中的修复合并回 `develop`。
