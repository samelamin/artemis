import {
  checkAndIncrementBudget,
  type BudgetCheckResult,
  type BudgetState,
} from "./dailyBudget";

export interface Env {
  REPORTS_BUCKET: R2Bucket;
  DAILY_BUDGET_DO: DurableObjectNamespace;
  REPORT_RATE_LIMITER: RateLimit;
}

// DurableObjectNamespace is used untyped above (no generic) because the
// generic form requires the DO class to be RPC-branded, which only happens
// by extending the cloudflare:workers DurableObject base class — and we
// deliberately do not extend that (see DailyBudgetCounter below). This
// interface describes the one custom RPC method callers actually invoke on
// the stub; the DurableObjectStub returned by .get() is cast to it below.
interface DailyBudgetStub {
  checkAndIncrement(
    day: string,
    incomingBytes: number,
    capBytes: number,
    capRequests: number,
  ): Promise<BudgetCheckResult>;
}

const CROCKFORD_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const REPORT_ID_PREFIX = "VBT-";
const REPORT_ID_LENGTH = 8;

const MAX_BODY_BYTES = 2 * 1024 * 1024;
const MAX_DAILY_BYTES = 8 * 1024 * 1024 * 1024;
const MAX_DAILY_REQUESTS = 4000;

const REPORT_PATH = "/v1/report";

// Durable Object that owns the daily budget counter. A plain class (does
// NOT extend the cloudflare:workers DurableObject base) — see the note at
// the top of this prompt / Wave 7 notes for why. All instances of this
// class share one logical counter per UTC day via a single, fixed DO id
// (see idFromName("global") in handlePutReport below), so storage grows by
// one small BudgetState row per calendar day — negligible, no expiry
// needed.
export class DailyBudgetCounter {
  private readonly state: DurableObjectState;

  constructor(state: DurableObjectState, _env: Env) {
    this.state = state;
  }

  async checkAndIncrement(
    day: string,
    incomingBytes: number,
    capBytes: number,
    capRequests: number,
  ): Promise<BudgetCheckResult> {
    return checkAndIncrementBudget(
      {
        get: (key) => this.state.storage.get<BudgetState>(key),
        put: (key, value) => this.state.storage.put<BudgetState>(key, value),
      },
      day,
      incomingBytes,
      capBytes,
      capRequests,
    );
  }
}

export function generateReportId(): string {
  const bytes = new Uint8Array(REPORT_ID_LENGTH);
  crypto.getRandomValues(bytes);
  let out = REPORT_ID_PREFIX;
  for (let i = 0; i < REPORT_ID_LENGTH; i++) {
    const value = bytes[i] ?? 0;
    const idx = value % CROCKFORD_ALPHABET.length;
    out += CROCKFORD_ALPHABET.charAt(idx);
  }
  return out;
}

export function objectKeyFor(id: string, version: string, now: Date): string {
  const yyyy = now.getUTCFullYear();
  const mm = String(now.getUTCMonth() + 1).padStart(2, "0");
  const dd = String(now.getUTCDate()).padStart(2, "0");
  const datePart = `${yyyy}-${mm}-${dd}`;
  const sanitized = version.replace(/[^A-Za-z0-9._-]/g, "");
  const containsDotDot = sanitized.includes("..");
  const versionPart =
    sanitized.length > 0 && !containsDotDot ? sanitized : "unknown";
  return `reports/${datePart}/${versionPart}/${id}.gz`;
}

export function dayKeyFor(now: Date): string {
  const yyyy = now.getUTCFullYear();
  const mm = String(now.getUTCMonth() + 1).padStart(2, "0");
  const dd = String(now.getUTCDate()).padStart(2, "0");
  return `${yyyy}-${mm}-${dd}`;
}

function jsonResponse(body: unknown, status: number): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function callerIp(request: Request): string {
  const ip = request.headers.get("CF-Connecting-IP");
  return ip && ip.length > 0 ? ip : "unknown";
}

export async function handlePutReport(
  request: Request,
  env: Env,
  now: Date = new Date(),
): Promise<Response> {
  if (request.method !== "PUT") {
    return jsonResponse({ error: "method not allowed" }, 405);
  }

  if (new URL(request.url).pathname !== REPORT_PATH) {
    return jsonResponse({ error: "not found" }, 404);
  }

  const contentLength = request.headers.get("content-length");
  if (contentLength !== null) {
    const declared = Number.parseInt(contentLength, 10);
    if (
      Number.isFinite(declared) &&
      declared > MAX_BODY_BYTES
    ) {
      return jsonResponse({ error: "body too large" }, 413);
    }
  }

  const ip = callerIp(request);
  const rate = await env.REPORT_RATE_LIMITER.limit({ key: ip });
  if (!rate.success) {
    return jsonResponse({ error: "rate limited" }, 429);
  }

  const buf = await request.arrayBuffer();
  if (buf.byteLength > MAX_BODY_BYTES) {
    return jsonResponse({ error: "body too large" }, 413);
  }

  const doId = env.DAILY_BUDGET_DO.idFromName("global");
  const budgetStub = env.DAILY_BUDGET_DO.get(doId) as unknown as DailyBudgetStub;
  const budget = await budgetStub.checkAndIncrement(
    dayKeyFor(now),
    buf.byteLength,
    MAX_DAILY_BYTES,
    MAX_DAILY_REQUESTS,
  );
  if (!budget.allowed) {
    return jsonResponse({ error: "daily budget exceeded" }, 503);
  }

  const id = generateReportId();
  const version = request.headers.get("X-Vbt-Ver") ?? "";
  const key = objectKeyFor(id, version, now);

  await env.REPORTS_BUCKET.put(key, buf);

  return jsonResponse({ id }, 200);
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      return await handlePutReport(request, env);
    } catch {
      return jsonResponse({ error: "internal error" }, 500);
    }
  },
};
