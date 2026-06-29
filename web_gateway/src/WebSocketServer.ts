/*
 * WebSocket 架构位置：
 * 本模块接收 Browser -> Gateway 的 /ws 长连接，完成 token 校验和 session 创建。
 * 当前采用“一 WebSocket 用户 -> 一 IMServer TCP 连接”的兼容模型：连接建立后先让
 * ImClient 以同一个 uid 登录 C++ IMServer，之后浏览器 send_message/message_ack
 * 分别转成 IMServer 的 CHAT_REQ/ACK，IMServer 的 ACK/CHAT_PUSH 再转回浏览器 JSON。
 */
import { randomUUID } from "crypto";
import { IncomingMessage, Server } from "http";
import { Duplex } from "stream";
import { RawData, WebSocket, WebSocketServer as WsServer } from "ws";

import { AuthService } from "./AuthService";
import { ImAckEvent, ImChatPushEvent, ImClient } from "./ImClient";
import { ImProtocol } from "./ImProtocol";
import { WsSession, WsSessionManager } from "./WsSessionManager";

type BrowserMessage = Record<string, unknown>;

type GatewayWebSocketOptions = {
  imHost: string;
  imPort: number;
  imProtocol: ImProtocol;
};

export class GatewayWebSocketServer {
  private readonly wsServer = new WsServer({ noServer: true });
  private readonly authService: AuthService;
  private readonly sessions: WsSessionManager;
  private readonly imHost: string;
  private readonly imPort: number;
  private readonly imProtocol: ImProtocol;
  private readonly imClientsBySessionId = new Map<string, ImClient>();

  constructor(server: Server, authService: AuthService, sessions: WsSessionManager, options: GatewayWebSocketOptions) {
    this.authService = authService;
    this.sessions = sessions;
    this.imHost = options.imHost;
    this.imPort = options.imPort;
    this.imProtocol = options.imProtocol;
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
      void this.attachSession(ws, auth.uid as number);
    });
  }

  private async attachSession(ws: WebSocket, uid: number): Promise<void> {
    const session = new WsSession(uid, randomUUID(), ws);
    const imClient = this.createImClient(session);
    this.imClientsBySessionId.set(session.sessionId, imClient);

    ws.on("message", (data) => this.handleMessage(session, data));
    ws.on("close", () => this.cleanupSession(session));
    ws.on("error", () => this.cleanupSession(session));

    try {
      await imClient.connectAndLogin();
    } catch (error) {
      const reason = error instanceof Error ? error.message : "failed to login IMServer";
      session.sendJson({ type: "error", reason });
      session.close(1011, reason);
      this.cleanupSession(session);
      return;
    }

    if (!session.isOpen()) {
      this.cleanupSession(session);
      return;
    }

    this.sessions.add(session);
    session.sendJson({
      type: "online",
      uid: session.uid,
      session_id: session.sessionId,
    });
  }

  private createImClient(session: WsSession): ImClient {
    return new ImClient({
      uid: session.uid,
      host: this.imHost,
      port: this.imPort,
      protocol: this.imProtocol,
      handlers: {
        onAck: (event) => this.forwardSendAck(session, event),
        onChatPush: (event) => this.forwardChatPush(session, event),
        onClose: (reason) => this.handleImClose(session, reason),
        onError: (reason) => this.handleImError(session, reason),
      },
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
    const imClient = this.imClientsBySessionId.get(session.sessionId);
    const toUid = Number(message.to_uid);
    const content = String(message.content || "").trim();
    const clientMsgId = String(message.client_msg_id || "");

    if (imClient === undefined) {
      session.sendJson({ type: "send_ack", ok: false, client_msg_id: clientMsgId, msg_id: 0, reason: "IMServer connection is not ready" });
      return;
    }
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

    try {
      imClient.sendChat(toUid, content, clientMsgId);
    } catch (error) {
      const reason = error instanceof Error ? error.message : "failed to send message to IMServer";
      session.sendJson({ type: "send_ack", ok: false, client_msg_id: clientMsgId, msg_id: 0, reason });
    }
  }

  private handleMessageAck(session: WsSession, message: BrowserMessage): void {
    const imClient = this.imClientsBySessionId.get(session.sessionId);
    const msgId = Number(message.msg_id);
    if (imClient === undefined || !Number.isInteger(msgId) || msgId <= 0) {
      return;
    }

    if (!imClient.ackMessage(msgId)) {
      session.sendJson({ type: "error", reason: `cannot ack unknown msg_id ${msgId}` });
    }
  }

  private forwardSendAck(session: WsSession, event: ImAckEvent): void {
    session.sendJson({
      type: "send_ack",
      ok: event.ok,
      client_msg_id: event.clientMsgId,
      msg_id: event.msgId,
      reason: event.reason,
    });
  }

  private forwardChatPush(session: WsSession, event: ImChatPushEvent): void {
    session.sendJson({
      type: "message",
      msg_id: event.msgId,
      from_uid: event.fromUid,
      to_uid: event.toUid,
      content: event.content,
      server_timestamp_ms: event.serverTimestampMs,
      history: event.history,
    });
  }

  private handleImClose(session: WsSession, reason: string): void {
    if (!session.isOpen()) {
      return;
    }
    session.sendJson({ type: "error", reason });
    session.close(1011, reason);
  }

  private handleImError(session: WsSession, reason: string): void {
    if (session.isOpen()) {
      session.sendJson({ type: "error", reason });
    }
  }

  private cleanupSession(session: WsSession): void {
    this.sessions.remove(session.uid, session.sessionId);
    const imClient = this.imClientsBySessionId.get(session.sessionId);
    if (imClient !== undefined) {
      this.imClientsBySessionId.delete(session.sessionId);
      imClient.close();
    }
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
