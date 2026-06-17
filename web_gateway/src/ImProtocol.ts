/*
 * IMServer 接入架构位置：
 * ImProtocol 是 Gateway 与 C++ IMServer 之间的协议适配层。浏览器侧使用 JSON，
 * IMServer 侧使用 4 字节网络序长度头 + Protobuf Envelope；本模块只负责构造、
 * 编码和解析 Envelope，不管理 TCP 连接和 WebSocket session。
 */
import protobuf from "protobufjs";

export type EnvelopeObject = Record<string, any>;

const MAX_FRAME_LENGTH = 1024 * 1024;

export class ImProtocol {
  private readonly envelopeType: protobuf.Type;

  private constructor(envelopeType: protobuf.Type) {
    this.envelopeType = envelopeType;
  }

  static async load(protoPath: string): Promise<ImProtocol> {
    const root = await protobuf.load(protoPath);
    return new ImProtocol(root.lookupType("message.Envelope"));
  }

  encode(envelope: EnvelopeObject): Buffer {
    const message = this.envelopeType.create(envelope);
    const body = Buffer.from(this.envelopeType.encode(message).finish());
    const frame = Buffer.allocUnsafe(4 + body.length);
    frame.writeUInt32BE(body.length, 0);
    body.copy(frame, 4);
    return frame;
  }

  decode(buffer: Buffer): { envelopes: EnvelopeObject[]; rest: Buffer } {
    const envelopes: EnvelopeObject[] = [];
    let offset = 0;

    while (buffer.length - offset >= 4) {
      const bodyLength = buffer.readUInt32BE(offset);
      if (bodyLength === 0 || bodyLength > MAX_FRAME_LENGTH) {
        throw new Error(`invalid frame length: ${bodyLength}`);
      }

      const frameLength = 4 + bodyLength;
      if (buffer.length - offset < frameLength) {
        break;
      }

      const body = buffer.subarray(offset + 4, offset + frameLength);
      const decoded = this.envelopeType.decode(body);
      envelopes.push(this.envelopeType.toObject(decoded, {
        enums: Number,
        longs: Number,
        defaults: false,
      }) as EnvelopeObject);
      offset += frameLength;
    }

    return { envelopes, rest: buffer.subarray(offset) };
  }

  createLoginRequest(seq: number, uid: number): EnvelopeObject {
    return {
      type: 1,
      seq,
      timestampMs: Date.now(),
      loginReq: { uid },
    };
  }

  createChatRequest(seq: number, fromUid: number, toUid: number, content: string, clientMsgId: string): EnvelopeObject {
    return {
      type: 3,
      seq,
      timestampMs: Date.now(),
      chatReq: {
        fromUid,
        toUid,
        content,
        clientMsgId,
      },
    };
  }

  createAck(seq: number, msgId: number, ok: boolean, reason: string): EnvelopeObject {
    return {
      type: 5,
      seq,
      timestampMs: Date.now(),
      ack: {
        msgId,
        seq,
        ok,
        reason,
      },
    };
  }
}
