/*
 * Gateway 入口架构位置：
 * server.ts 是 Web Gateway 的组合入口，负责同时承载静态前端页面、HTTP 登录/注册
 * 接口，以及 /ws WebSocket Upgrade。WebSocket 模块会为每个在线 Web 用户创建一条
 * 到 C++ IMServer 的 TCP/Protobuf 连接，让 IMServer 继续保持 uid -> tcp connection 模型。
 */
import { createReadStream, promises as fsPromises } from "fs";
import { createServer, IncomingMessage, ServerResponse } from "http";
import { extname, join, normalize, resolve } from "path";
import { AuthService } from "./AuthService";
import { ImProtocol } from "./ImProtocol";
import { GatewayWebSocketServer } from "./WebSocketServer";
import { WsSessionManager } from "./WsSessionManager";

const HOST = process.env.WEB_GATEWAY_HOST || "127.0.0.1";
const PORT = Number(process.env.WEB_GATEWAY_PORT || 3000);
const TOKEN_TTL_MS = Number(process.env.TOKEN_TTL_MS || 2 * 60 * 60 * 1000);
const IM_HOST = process.env.IM_HOST || "127.0.0.1";
const IM_PORT = Number(process.env.IM_PORT || 8080);
const PUBLIC_DIR = resolve(__dirname, "..", "public");
const PROTO_PATH = resolve(__dirname, "..", "..", "protocol", "Message.proto");

const authService = new AuthService(TOKEN_TTL_MS);
const sessionManager = new WsSessionManager();

type JsonObject = Record<string, unknown>;

const server = createServer(async (req, res) => {
  try {
    if (req.method === "POST" && req.url === "/api/register") {
      await handleRegister(req, res);
      return;
    }

    if (req.method === "POST" && req.url === "/api/login") {
      await handleLogin(req, res);
      return;
    }

    if (req.method === "GET") {
      await serveStatic(req, res);
      return;
    }

    sendJson(res, 404, { ok: false, reason: "not found" });
  } catch (error) {
    const reason = error instanceof Error ? error.message : "internal server error";
    sendJson(res, 500, { ok: false, reason });
  }
});

async function handleRegister(req: IncomingMessage, res: ServerResponse): Promise<void> {
  const body = await readJson(req);
  const userId = Number(body.user_id);
  const password = String(body.password || "");
  const result = authService.register(userId, password);
  sendJson(res, result.ok ? 200 : 400, result);
}

async function handleLogin(req: IncomingMessage, res: ServerResponse): Promise<void> {
  const body = await readJson(req);
  const userId = Number(body.user_id);
  const password = String(body.password || "");
  const result = authService.login(userId, password);
  sendJson(res, result.ok ? 200 : 401, result);
}

async function readJson(req: IncomingMessage): Promise<JsonObject> {
  const chunks: Buffer[] = [];
  for await (const chunk of req) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }

  if (chunks.length === 0) {
    return {};
  }

  try {
    return JSON.parse(Buffer.concat(chunks).toString("utf8")) as JsonObject;
  } catch {
    throw new Error("invalid json body");
  }
}

async function serveStatic(req: IncomingMessage, res: ServerResponse): Promise<void> {
  const requestUrl = new URL(req.url || "/", `http://${HOST}:${PORT}`);
  const pathname = requestUrl.pathname === "/" ? "/index.html" : requestUrl.pathname;
  const relativePath = normalize(decodeURIComponent(pathname)).replace(/^[/\\]+/, "");
  if (relativePath.split(/[/\\]+/).includes("..")) {
    sendJson(res, 403, { ok: false, reason: "forbidden" });
    return;
  }

  const filePath = resolve(join(PUBLIC_DIR, relativePath));

  if (!filePath.startsWith(PUBLIC_DIR)) {
    sendJson(res, 403, { ok: false, reason: "forbidden" });
    return;
  }

  try {
    const stat = await fsPromises.stat(filePath);
    if (!stat.isFile()) {
      sendJson(res, 404, { ok: false, reason: "not found" });
      return;
    }
  } catch {
    sendJson(res, 404, { ok: false, reason: "not found" });
    return;
  }

  res.writeHead(200, { "Content-Type": contentType(filePath) });
  createReadStream(filePath).pipe(res);
}

function sendJson(res: ServerResponse, statusCode: number, data: JsonObject): void {
  const body = JSON.stringify(data);
  res.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": Buffer.byteLength(body),
  });
  res.end(body);
}

function contentType(filePath: string): string {
  switch (extname(filePath)) {
    case ".html":
      return "text/html; charset=utf-8";
    case ".js":
      return "text/javascript; charset=utf-8";
    case ".css":
      return "text/css; charset=utf-8";
    case ".json":
      return "application/json; charset=utf-8";
    default:
      return "application/octet-stream";
  }
}

async function main(): Promise<void> {
  const imProtocol = await ImProtocol.load(PROTO_PATH);
  new GatewayWebSocketServer(server, authService, sessionManager, {
    imHost: IM_HOST,
    imPort: IM_PORT,
    imProtocol,
  });

  server.listen(PORT, HOST, () => {
    console.log(`web gateway listening on http://${HOST}:${PORT}`);
    console.log(`IMServer target ${IM_HOST}:${IM_PORT}`);
  });
}

main().catch((error) => {
  const reason = error instanceof Error ? error.message : String(error);
  console.error(`failed to start web gateway: ${reason}`);
  process.exit(1);
});
