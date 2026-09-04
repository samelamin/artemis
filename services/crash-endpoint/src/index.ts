export interface Env {
  REPORTS_BUCKET: R2Bucket;
  DAILY_BUDGET_KV: KVNamespace;
  REPORT_RATE_LIMITER: RateLimit;
}

const CROCKFORD_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const REPORT_ID_PREFIX = "VBT-";
const REPORT_ID_LENGTH = 8;

const MAX_BODY_BYTES = 2 * 1024 * 1024;
const MAX_DAILY_BYTES = 8 * 1024 * 1024 * 1024;
const MAX_DAILY_REQUESTS = 4000;

const REPORT_PATH = "/v1/report";

interface BudgetState {
  bytes: number;
  requests: number;
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

export function budgetKeyFor(now: Date): string {
  const yyyy = now.getUTCFullYear();
  const mm = String(now.getUTCMonth() + 1).padStart(2, "0");
  const dd = String(now.getUTCDate()).padStart(2, "0");
  return `budget:${yyyy}-${mm}-${dd}`;
}

export async function checkDailyBudget(
  kv: KVNamespace,
  incomingBytes: number,
  now: Date = new Date(),
): Promise<{ allowed: boolean; bytesToday: number; requestsToday: number }> {
  const key = budgetKeyFor(now);
  const raw = await kv.get(key);
  const state = parseBudgetState(raw);
  const projectedBytes = state.bytes + incomingBytes;
  const projectedRequests = state.requests + 1;
  const allowed =
    projectedBytes <= MAX_DAILY_BYTES && projectedRequests <= MAX_DAILY_REQUESTS;
  return {
    allowed,
    bytesToday: state.bytes,
    requestsToday: state.requests,
  };
}

export async function recordDailyUsage(
  kv: KVNamespace,
  bytes: number,
  now: Date = new Date(),
): Promise<void> {
  const key = budgetKeyFor(now);
  const raw = await kv.get(key);
  const state = parseBudgetState(raw);
  state.bytes += bytes;
  state.requests += 1;
  await kv.put(key, JSON.stringify(state));
}

function parseBudgetState(raw: string | null): BudgetState {
  if (raw === null) {
    return { bytes: 0, requests: 0 };
  }
  try {
    const parsed = JSON.parse(raw) as Partial<BudgetState>;
    const bytes =
      typeof parsed.bytes === "number" && Number.isFinite(parsed.bytes)
        ? parsed.bytes
        : 0;
    const requests =
      typeof parsed.requests === "number" && Number.isFinite(parsed.requests)
        ? Math.max(0, Math.floor(parsed.requests))
        : 0;
    return { bytes, requests };
  } catch {
    return { bytes: 0, requests: 0 };
  }
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

  const buf = await request.arrayBuffer();
  if (buf.byteLength > MAX_BODY_BYTES) {
    return jsonResponse({ error: "body too large" }, 413);
  }

  const ip = callerIp(request);
  const rate = await env.REPORT_RATE_LIMITER.limit({ key: ip });
  if (!rate.success) {
    return jsonResponse({ error: "rate limited" }, 429);
  }

  const budget = await checkDailyBudget(env.DAILY_BUDGET_KV, buf.byteLength, now);
  if (!budget.allowed) {
    return jsonResponse({ error: "daily budget exceeded" }, 503);
  }

  const id = generateReportId();
  const version = request.headers.get("X-Vbt-Ver") ?? "";
  const key = objectKeyFor(id, version, now);

  await env.REPORTS_BUCKET.put(key, buf);
  await recordDailyUsage(env.DAILY_BUDGET_KV, buf.byteLength, now);

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
