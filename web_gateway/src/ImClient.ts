/*
 * IMServer 接入架构位置：
 * ImClient 表示 Gateway 代一个浏览器用户建立的一条 IMServer TCP 连接。当前阶段
 * 采用“一 WebSocket 用户 -> 一 IMServer TCP 连接”的兼容方案，因此 C++ IMServer
 * 仍可保持 uid -> tcp connection 的旧模型，不需要改成 gateway 多路复用协议。
 */
import { Socket } from "net";

import { EnvelopeObject, ImProtocol } from "./ImProtocol";

type PendingSend = {
  clientMsgId: string;
};

export type ImAckEvent = {
  clientMsgId: string;
  msgId: number;
  ok: boolean;
  reason: string;
};

export type ImChatPushEvent = {
  msgId: number;
  seq: number;
  fromUid: number;
  toUid: number;
  content: string;
  serverTimestampMs: number;
  history: boolean;
};

export type ImClientHandlers = {
  onAck: (event: ImAckEvent) => void;
  onChatPush: (event: ImChatPushEvent) => void;
  onClose: (reason: string) => void;
  onError: (reason: string) => void;
};

export type ImClientOptions = {
  uid: number;
  host: string;
  port: number;
  protocol: ImProtocol;
  handlers: ImClientHandlers;
};

export class ImClient {
  private readonly uid: number;
  private readonly host: string;
  private readonly port: number;
  private readonly protocol: ImProtocol;
  private readonly handlers: ImClientHandlers;
  private readonly pendingSends = new Map<number, PendingSend>();
  private readonly pushSeqByMsgId = new Map<number, number>();
  private socket: Socket | null = null;
  private recvBuffer: Buffer<ArrayBufferLike> = Buffer.alloc(0);
  private nextSeq = 1;
  private connected = false;
  private loginResolver: ((value: void) => void) | null = null;
  private loginRejecter: ((reason: Error) => void) | null = null;

  constructor(options: ImClientOptions) {
    this.uid = options.uid;
    this.host = options.host;
    this.port = options.port;
    this.protocol = options.protocol;
    this.handlers = options.handlers;
  }

  connectAndLogin(): Promise<void> {
    if (this.socket !== null) {
      return Promise.reject(new Error("IM client already started"));
    }

    return new Promise((resolve, reject) => {
      this.loginResolver = resolve;
      this.loginRejecter = reject;

      const socket = new Socket();
      this.socket = socket;

      socket.on("connect", () => {
        this.connected = true;
        this.sendEnvelope(this.protocol.createLoginRequest(this.allocateSeq(), this.uid));
      });
      socket.on("data", (chunk) => this.handleData(chunk));
      socket.on("close", () => this.handleClose("IMServer connection closed"));
      socket.on("error", (error) => this.handleError(error.message));
      socket.connect(this.port, this.host);
    });
  }

  sendChat(toUid: number, content: string, clientMsgId: string): void {
    const seq = this.allocateSeq();
    this.pendingSends.set(seq, { clientMsgId });
    this.sendEnvelope(this.protocol.createChatRequest(seq, this.uid, toUid, content, clientMsgId));
  }

  ackMessage(msgId: number): boolean {
    const seq = this.pushSeqByMsgId.get(msgId);
    if (seq === undefined) {
      return false;
    }
    this.pushSeqByMsgId.delete(msgId);
    this.sendEnvelope(this.protocol.createAck(seq, msgId, true, ""));
    return true;
  }

  close(): void {
    if (this.socket === null) {
      return;
    }
    this.socket.destroy();
    this.socket = null;
    this.connected = false;
  }

  private handleData(chunk: Buffer): void {
    this.recvBuffer = Buffer.concat([this.recvBuffer, chunk]);

    let decoded;
    try {
      decoded = this.protocol.decode(this.recvBuffer);
    } catch (error) {
      const reason = error instanceof Error ? error.message : "decode failed";
      this.handleError(reason);
      this.close();
      return;
    }

    this.recvBuffer = decoded.rest;
    for (const envelope of decoded.envelopes) {
      this.handleEnvelope(envelope);
    }
  }

  private handleEnvelope(envelope: EnvelopeObject): void {
    if (envelope.type === 2) {
      this.handleLoginResponse(envelope);
      return;
    }
    if (envelope.type === 5) {
      this.handleAck(envelope);
      return;
    }
    if (envelope.type === 4) {
      this.handleChatPush(envelope);
    }
  }

  private handleLoginResponse(envelope: EnvelopeObject): void {
    const response = envelope.loginResp || {};
    if (response.ok === true) {
      this.loginResolver?.();
    } else {
      this.loginRejecter?.(new Error(String(response.reason || "login rejected by IMServer")));
    }
    this.loginResolver = null;
    this.loginRejecter = null;
  }

  private handleAck(envelope: EnvelopeObject): void {
    const ack = envelope.ack || {};
    const seq = toNumber(ack.seq);
    const pending = this.pendingSends.get(seq);
    if (pending === undefined) {
      return;
    }

    this.pendingSends.delete(seq);
    this.handlers.onAck({
      clientMsgId: pending.clientMsgId,
      msgId: toNumber(ack.msgId),
      ok: ack.ok === true,
      reason: String(ack.reason || ""),
    });
  }

  private handleChatPush(envelope: EnvelopeObject): void {
    const push = envelope.chatPush || {};
    const msgId = toNumber(push.msgId);
    const seq = toNumber(envelope.seq);
    const history = push.history === true;
    if (!history) {
      this.pushSeqByMsgId.set(msgId, seq);
    }

    this.handlers.onChatPush({
      msgId,
      seq,
      fromUid: toNumber(push.fromUid),
      toUid: toNumber(push.toUid),
      content: String(push.content || ""),
      serverTimestampMs: toNumber(push.serverTimestampMs) || Date.now(),
      history,
    });
  }

  private sendEnvelope(envelope: EnvelopeObject): void {
    if (!this.connected || this.socket === null) {
      throw new Error("IMServer connection is not ready");
    }
    this.socket.write(this.protocol.encode(envelope));
  }

  private allocateSeq(): number {
    return this.nextSeq++;
  }

  private handleClose(reason: string): void {
    this.connected = false;
    this.rejectPendingLogin(reason);
    this.handlers.onClose(reason);
  }

  private handleError(reason: string): void {
    this.rejectPendingLogin(reason);
    this.handlers.onError(reason);
  }

  private rejectPendingLogin(reason: string): void {
    if (this.loginRejecter !== null) {
      this.loginRejecter(new Error(reason));
      this.loginResolver = null;
      this.loginRejecter = null;
    }
  }
}

function toNumber(value: unknown): number {
  if (typeof value === "number") {
    return value;
  }
  if (typeof value === "string") {
    return Number(value);
  }
  if (value !== null && typeof value === "object" && "toNumber" in value && typeof value.toNumber === "function") {
    return value.toNumber();
  }
  return 0;
}
