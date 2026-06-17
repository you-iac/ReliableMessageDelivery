/*
 * WebSocket 架构位置：
 * 本文件合并管理单个 WebSocket 连接和在线连接表。WsSession 表示 Browser ->
 * Gateway 的一条 /ws 长连接；WsSessionManager 维护 uid -> session 的映射，
 * 用于判断谁在线、向指定 uid 推送消息，并避免旧连接 close 误删新连接。
 */
import { WebSocket } from "ws";

export type JsonPayload = Record<string, unknown>;

export class WsSession {
  readonly uid: number;
  readonly sessionId: string;
  private readonly ws: WebSocket;

  constructor(uid: number, sessionId: string, ws: WebSocket) {
    this.uid = uid;
    this.sessionId = sessionId;
    this.ws = ws;
  }

  isOpen(): boolean {
    return this.ws.readyState === WebSocket.OPEN;
  }

  sendJson(payload: JsonPayload): void {
    if (!this.isOpen()) {
      return;
    }
    this.ws.send(JSON.stringify(payload));
  }

  close(code = 1000, reason = "session closed"): void {
    if (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING) {
      this.ws.close(code, reason);
    }
  }
}

export class WsSessionManager {
  private readonly sessionsByUid = new Map<number, WsSession>();
  private readonly sessionsById = new Map<string, WsSession>();

  add(session: WsSession): void {
    const oldSession = this.sessionsByUid.get(session.uid);
    if (oldSession !== undefined && oldSession.sessionId !== session.sessionId) {
      this.remove(oldSession.uid, oldSession.sessionId);
      oldSession.close(1000, "replaced by a newer session");
    }

    this.sessionsByUid.set(session.uid, session);
    this.sessionsById.set(session.sessionId, session);
  }

  remove(uid: number, sessionId: string): void {
    const current = this.sessionsByUid.get(uid);
    if (current !== undefined && current.sessionId === sessionId) {
      this.sessionsByUid.delete(uid);
    }
    this.sessionsById.delete(sessionId);
  }

  getByUid(uid: number): WsSession | undefined {
    const session = this.sessionsByUid.get(uid);
    if (session === undefined || !session.isOpen()) {
      return undefined;
    }
    return session;
  }
}
