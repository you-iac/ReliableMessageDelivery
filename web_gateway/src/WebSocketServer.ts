/*
 * WebSocket 架构位置：
 * 本模块接收 Browser -> Gateway 的 /ws 长连接，完成 token 校验、session 创建、
 * 浏览器 JSON 消息解析和 mock 投递。后续接入 IMServer 时，send_message 和
 * message_ack 会从这里转交给 ImClient，再由 IMServer 负责可靠投递和 ACK 状态。
 */
import { randomUUID } from "crypto";
import { IncomingMessage, Server } from "http";
import { Duplex } from "stream";
import { RawData, WebSocket, WebSocketServer as WsServer } from "ws";

import { AuthService } from "./AuthService";
import { WsSession, WsSessionManager } from "./WsSessionManager";

type BrowserMessage = Record<string, unknown>;

export class GatewayWebSocketServer {
  private readonly wsServer = new WsServer({ noServer: true });
  private readonly authService: AuthService;
  private readonly sessions: WsSessionManager;
  private nextMsgId = 1000;

  constructor(server: Server, authService: AuthService, sessions: WsSessionManager) {
    this.authService = authService;
    this.sessions = sessions;
    server.on("upgrade", (req, socket, head) => this.handleUpgrade(req, socket, head));
  }

  private handleUpgrade(req: IncomingMessage, socket: Duplex, head: Buffer): void {
    const requestUrl = new URL(req.url || "/", "http://localhost");
    if (requestUrl.pathname !== "/ws") {
      this.rejectUpgrade(socket, 404, "not found");
      return;
    }

    const token = requestUrl.searchParams.get("token") || "";
    const auth = this.authService.verifyToken(token);
    if (!auth.ok || auth.uid === undefined) {
      this.rejectUpgrade(socket, 401, auth.reason || "invalid token");
      return;
    }

    this.wsServer.handleUpgrade(req, socket, head, (ws) => {
      this.attachSession(ws, auth.uid as number);
    });
  }

  private attachSession(ws: WebSocket, uid: number): void {
    const session = new WsSession(uid, randomUUID(), ws);
    this.sessions.add(session);

    session.sendJson({
      type: "online",
      uid: session.uid,
      session_id: session.sessionId,
    });

    ws.on("message", (data) => this.handleMessage(session, data));
    ws.on("close", () => {
      this.sessions.remove(session.uid, session.sessionId);
    });
    ws.on("error", () => {
      this.sessions.remove(session.uid, session.sessionId);
    });
  }

  private handleMessage(session: WsSession, data: RawData): void {
    const message = this.parseMessage(data);
    if (message === null) {
      session.sendJson({ type: "error", reason: "invalid json message" });
      return;
    }

    if (message.type === "send_message") {
      this.handleSendMessage(session, message);
      return;
    }

    if (message.type === "message_ack") {
      this.handleMessageAck(session, message);
      return;
    }

    if (message.type === "ping") {
      session.sendJson({ type: "pong" });
      return;
    }

    session.sendJson({ type: "error", reason: "unknown message type" });
  }

  private handleSendMessage(session: WsSession, message: BrowserMessage): void {
    const toUid = Number(message.to_uid);
    const content = String(message.content || "").trim();
    const clientMsgId = String(message.client_msg_id || "");

    if (!Number.isInteger(toUid) || toUid <= 0) {
      session.sendJson({ type: "send_ack", ok: false, client_msg_id: clientMsgId, msg_id: 0, reason: "invalid to_uid" });
      return;
    }
    if (content.length === 0) {
      session.sendJson({ type: "send_ack", ok: false, client_msg_id: clientMsgId, msg_id: 0, reason: "empty content" });
      return;
    }
    if (clientMsgId.length === 0) {
      session.sendJson({ type: "send_ack", ok: false, client_msg_id: clientMsgId, msg_id: 0, reason: "missing client_msg_id" });
      return;
    }

    const msgId = this.nextMsgId++;
    const now = Date.now();
    session.sendJson({
      type: "send_ack",
      ok: true,
      client_msg_id: clientMsgId,
      msg_id: msgId,
    });

    const receiver = this.sessions.getByUid(toUid);
    if (receiver === undefined) {
      session.sendJson({ type: "error", reason: `uid ${toUid} is offline in mock gateway` });
      return;
    }

    receiver.sendJson({
      type: "message",
      msg_id: msgId,
      from_uid: session.uid,
      to_uid: toUid,
      content,
      server_timestamp_ms: now,
    });
  }

  private handleMessageAck(_session: WsSession, message: BrowserMessage): void {
    const msgId = Number(message.msg_id);
    if (!Number.isInteger(msgId) || msgId <= 0) {
      return;
    }
    // 第一版 mock gateway 暂不持久化 ACK；接入 IMServer 后这里转发 USER_ACK。
  }

  private parseMessage(data: RawData): BrowserMessage | null {
    try {
      const raw = Array.isArray(data) ? Buffer.concat(data).toString("utf8") : data.toString();
      const parsed = JSON.parse(raw) as unknown;
      if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
        return null;
      }
      return parsed as BrowserMessage;
    } catch {
      return null;
    }
  }

  private rejectUpgrade(socket: Duplex, statusCode: number, reason: string): void {
    socket.write(`HTTP/1.1 ${statusCode} ${reason}
Connection: close

`);
    socket.destroy();
  }
}
