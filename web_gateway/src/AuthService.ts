import { randomBytes } from "crypto";

export type AuthResult = {
  ok: boolean;
  uid?: number;
  token?: string;
  reason?: string;
};

type TokenRecord = {
  uid: number;
  expiresAtMs: number;
};

export class AuthService {
  private readonly users = new Map<number, string>();
  private readonly tokens = new Map<string, TokenRecord>();
  private readonly tokenTtlMs: number;

  constructor(tokenTtlMs: number) {
    this.tokenTtlMs = tokenTtlMs;
  }

  register(userId: number, password: string): AuthResult {
    const validation = this.validateCredentials(userId, password);
    if (validation !== "") {
      return { ok: false, reason: validation };
    }
    if (this.users.has(userId)) {
      return { ok: false, reason: "user already exists" };
    }
    if (String(userId) !== password) {
      return { ok: false, reason: "first version requires account equals password" };
    }

    this.users.set(userId, password);
    return { ok: true, uid: userId };
  }

  login(userId: number, password: string): AuthResult {
    const validation = this.validateCredentials(userId, password);
    if (validation !== "") {
      return { ok: false, reason: validation };
    }

    const savedPassword = this.users.get(userId);
    const passwordMatchesSavedUser = savedPassword !== undefined && savedPassword === password;
    const passwordMatchesDemoRule = savedPassword === undefined && String(userId) === password;
    if (!passwordMatchesSavedUser && !passwordMatchesDemoRule) {
      return { ok: false, reason: "invalid user_id or password" };
    }

    if (savedPassword === undefined) {
      this.users.set(userId, password);
    }

    const token = this.createToken();
    this.tokens.set(token, {
      uid: userId,
      expiresAtMs: Date.now() + this.tokenTtlMs,
    });

    return { ok: true, uid: userId, token };
  }

  verifyToken(token: string): AuthResult {
    const record = this.tokens.get(token);
    if (record === undefined) {
      return { ok: false, reason: "invalid token" };
    }
    if (Date.now() > record.expiresAtMs) {
      this.tokens.delete(token);
      return { ok: false, reason: "token expired" };
    }
    return { ok: true, uid: record.uid, token };
  }

  private validateCredentials(userId: number, password: string): string {
    if (!Number.isInteger(userId) || userId <= 0) {
      return "user_id must be a positive integer";
    }
    if (password.length === 0) {
      return "password is required";
    }
    return "";
  }

  private createToken(): string {
    return randomBytes(24).toString("hex");
  }
}
