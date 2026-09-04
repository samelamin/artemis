import { describe, it, expect, beforeEach } from "vitest";
import {
  handlePutReport,
  generateReportId,
  objectKeyFor,
  dayKeyFor,
  type Env,
} from "../src/index";
import worker from "../src/index";
import {
  checkAndIncrementBudget,
  type BudgetState,
  type BudgetCheckResult,
} from "../src/dailyBudget";

// In-memory storage backing FakeDailyBudgetStub. get()/put() are plain,
// unqueued async methods — the atomicity guarantee lives one level up, in
// FakeDailyBudgetStub.checkAndIncrement's runExclusive() wrapper below,
// which serializes the *entire* get-compute-put operation as one unit.
// (An earlier version of this fake queued get() and put() individually,
// which does not model the real thing: two concurrent operations could
// still interleave as A.get, B.get, A.put, B.put, with both reading the
// same pre-increment state before either writes. The Durable Object
// "input gate" this fake exists to mirror defers a whole incoming RPC
// invocation — not individual storage calls — until the previous one
// finishes, which is why the serialization boundary belongs around the
// whole operation.)
class FakeSerializedStorage {
  private map = new Map<string, BudgetState>();
  private queue: Promise<unknown> = Promise.resolve();

  // Runs fn() exclusively with respect to every other call queued through
  // this same instance. Models the Durable Object "input gate": the whole
  // RPC invocation (get, compute, put, with no other yielding I/O in
  // between) runs to completion before the next queued invocation starts.
  runExclusive<T>(fn: () => Promise<T>): Promise<T> {
    const result = this.queue.then(fn);
    this.queue = result.catch(() => undefined);
    return result;
  }

  async get(key: string): Promise<BudgetState | undefined> {
    return this.map.get(key);
  }

  async put(key: string, value: BudgetState): Promise<void> {
    this.map.set(key, value);
  }
}

class FakeDailyBudgetStub {
  storage = new FakeSerializedStorage();

  async checkAndIncrement(
    day: string,
    incomingBytes: number,
    capBytes: number,
    capRequests: number,
  ): Promise<BudgetCheckResult> {
    return this.storage.runExclusive(() =>
      checkAndIncrementBudget(
        this.storage,
        day,
        incomingBytes,
        capBytes,
        capRequests,
      ),
    );
  }

  // Test helper only — seeds the day's counter directly, the way
  // globalDailyBudget tests need to start "already near the cap". Not
  // routed through runExclusive: it runs before any concurrent traffic
  // exists in every test that uses it.
  async seed(day: string, state: BudgetState): Promise<void> {
    await this.storage.put(`budget:${day}`, state);
  }
}

class FakeDailyBudgetNamespace {
  stub = new FakeDailyBudgetStub();
  idFromName(_name: string): string {
    return "global";
  }
  get(_id: string): FakeDailyBudgetStub {
    return this.stub;
  }
}

class FakeRateLimiter {
  success = true;
  calls: Array<{ key: string }> = [];
  async limit(options: { key: string }): Promise<{ success: boolean }> {
    this.calls.push({ key: options.key });
    return { success: this.success };
  }
}

interface StoredObject {
  key: string;
  byteLength: number;
}

class FakeR2Bucket {
  objects: StoredObject[] = [];
  async put(
    key: string,
    value:
      | ArrayBuffer
      | Uint8Array
      | string
      | ReadableStream
      | Blob
      | null,
  ): Promise<void> {
    let byteLength = 0;
    if (value instanceof ArrayBuffer) {
      byteLength = value.byteLength;
    } else if (value instanceof Uint8Array) {
      byteLength = value.byteLength;
    } else if (typeof value === "string") {
      byteLength = new TextEncoder().encode(value).byteLength;
    } else if (value instanceof Blob) {
      byteLength = value.size;
    }
    this.objects.push({ key, byteLength });
  }
}

interface TestEnv {
  env: Env;
  budgetDO: FakeDailyBudgetNamespace;
  limiter: FakeRateLimiter;
  r2: FakeR2Bucket;
}

function makeEnv(): TestEnv {
  const budgetDO = new FakeDailyBudgetNamespace();
  const limiter = new FakeRateLimiter();
  const r2 = new FakeR2Bucket();
  const env: Env = {
    REPORTS_BUCKET: r2 as unknown as R2Bucket,
    DAILY_BUDGET_DO: budgetDO as unknown as DurableObjectNamespace,
    REPORT_RATE_LIMITER: limiter as unknown as RateLimit,
  };
  return { env, budgetDO, limiter, r2 };
}

const NOW = new Date("2026-09-04T12:00:00Z");

function utcDateString(d: Date): string {
  const yyyy = d.getUTCFullYear();
  const mm = String(d.getUTCMonth() + 1).padStart(2, "0");
  const dd = String(d.getUTCDate()).padStart(2, "0");
  return `${yyyy}-${mm}-${dd}`;
}

describe("generateReportId", () => {
  it("idShape: matches Crockford base32 shape for every call", () => {
    const re = /^VBT-[0-9A-HJKMNP-TV-Z]{8}$/;
    for (let i = 0; i < 200; i++) {
      const id = generateReportId();
      expect(id).toMatch(re);
      expect(id).toHaveLength(12);
      expect(id.startsWith("VBT-")).toBe(true);
    }
  });

  it("excludes visually confusable letters I, L, O, U", () => {
    for (let i = 0; i < 500; i++) {
      const id = generateReportId();
      const tail = id.slice(4);
      expect(tail).not.toMatch(/[ILOU]/);
    }
  });
});

describe("objectKeyFor", () => {
  it("objectKeyShape: normal version produces expected path", () => {
    const key = objectKeyFor("VBT-12345678", "1.2.3", NOW);
    expect(key).toBe("reports/2026-09-04/1.2.3/VBT-12345678.gz");
  });

  it("objectKeyShape: empty version falls back to 'unknown'", () => {
    const key = objectKeyFor("VBT-12345678", "", NOW);
    expect(key).toBe("reports/2026-09-04/unknown/VBT-12345678.gz");
  });

  it("objectKeyShape: malicious version with ../ is rejected", () => {
    const key = objectKeyFor("VBT-12345678", "../../etc/passwd", NOW);
    expect(key).not.toContain("..");
    expect(key).not.toContain("/etc/passwd");
    expect(key.startsWith("reports/2026-09-04/")).toBe(true);
    expect(key.endsWith("/VBT-12345678.gz")).toBe(true);
  });

  it("objectKeyShape: malicious version with / and backslash stripped", () => {
    const key = objectKeyFor("VBT-12345678", "a/b\\c", NOW);
    expect(key).not.toContain("\\");
    const parts = key.split("/");
    expect(parts).toHaveLength(4);
    expect(parts[0]).toBe("reports");
    expect(parts[1]).toBe("2026-09-04");
    expect(parts[2]).toBe("abc");
    expect(parts[3]).toBe("VBT-12345678.gz");
  });

  it("objectKeyShape: malicious version containing a literal '..' but no slashes still rejected", () => {
    const key = objectKeyFor("VBT-12345678", "foo..bar", NOW);
    expect(key).not.toContain("..");
    expect(key).toBe("reports/2026-09-04/unknown/VBT-12345678.gz");
  });
});

describe("handler", () => {
  let ctx: TestEnv;
  beforeEach(() => {
    ctx = makeEnv();
  });

  it("methodRejection: GET to /v1/report returns 405", async () => {
    const req = new Request("https://example.com/v1/report", {
      method: "GET",
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(405);
    expect(ctx.r2.objects).toHaveLength(0);
  });

  it("methodRejection: POST to /v1/report returns 405", async () => {
    const req = new Request("https://example.com/v1/report", {
      method: "POST",
      body: "x",
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(405);
    expect(ctx.r2.objects).toHaveLength(0);
  });

  it("pathRejection: PUT to /other returns 404", async () => {
    const req = new Request("https://example.com/other", {
      method: "PUT",
      body: new Uint8Array([1, 2, 3]),
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(404);
    expect(ctx.r2.objects).toHaveLength(0);
  });

  it("sizeRejection: body over 2 MB returns 413 and does not call R2", async () => {
    const big = new Uint8Array(3 * 1024 * 1024);
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      body: big,
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(413);
    expect(ctx.r2.objects).toHaveLength(0);
  });

  it("sizeRejection: Content-Length over 2 MB rejects before reading body", async () => {
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      headers: { "content-length": String(3 * 1024 * 1024) },
      body: new Uint8Array([1]),
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(413);
    expect(ctx.r2.objects).toHaveLength(0);
  });

  it("perIpRateLimit: rate limiter rejects returns 429 and does not call R2", async () => {
    ctx.limiter.success = false;
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      headers: { "CF-Connecting-IP": "1.2.3.4" },
      body: new Uint8Array([1, 2, 3]),
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(429);
    expect(ctx.r2.objects).toHaveLength(0);
    expect(ctx.limiter.calls).toEqual([{ key: "1.2.3.4" }]);
  });

  it("perIpRateLimit: rejects a request with no Content-Length before buffering the body", async () => {
    ctx.limiter.success = false;
    const stream = new ReadableStream({
      start(controller) {
        controller.enqueue(new Uint8Array([1, 2, 3]));
        // Intentionally never closes — if the handler tried to fully
        // buffer this body it would hang forever.
      },
    });
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      headers: { "CF-Connecting-IP": "9.9.9.9" },
      // @ts-expect-error duplex is required by the fetch spec for
      // streaming bodies but is missing from this Request typing.
      duplex: "half",
      body: stream,
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(429);
    expect(ctx.r2.objects).toHaveLength(0);
  });

  it("globalDailyBudget: bytes already at cap returns 503 and does not call R2", async () => {
    await ctx.budgetDO.stub.seed(dayKeyFor(NOW), {
      bytes: 8 * 1024 * 1024 * 1024,
      requests: 1,
    });
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      body: new Uint8Array([1, 2, 3]),
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(503);
    expect(ctx.r2.objects).toHaveLength(0);
  });

  it("globalDailyBudget: requests already at cap returns 503 and does not call R2", async () => {
    await ctx.budgetDO.stub.seed(dayKeyFor(NOW), { bytes: 0, requests: 4000 });
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      body: new Uint8Array([1, 2, 3]),
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(503);
    expect(ctx.r2.objects).toHaveLength(0);
  });

  it("successStoresAndReturnsId: stores with the right key and byte count, returns id", async () => {
    const payload = new TextEncoder().encode("hello world");
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      headers: { "X-Vbt-Ver": "1.2.3" },
      body: payload,
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toBe("application/json");
    const body = (await res.json()) as { id: string };
    expect(body.id).toMatch(/^VBT-[0-9A-HJKMNP-TV-Z]{8}$/);
    expect(ctx.r2.objects).toHaveLength(1);
    const obj = ctx.r2.objects[0]!;
    expect(obj.byteLength).toBe(payload.byteLength);
    const expectedPrefix = `reports/${utcDateString(NOW)}/1.2.3/`;
    expect(obj.key.startsWith(expectedPrefix)).toBe(true);
    expect(obj.key.endsWith(".gz")).toBe(true);
    expect(obj.key.slice(expectedPrefix.length, -3)).toBe(body.id);
  });

  it("successStoresAndReturnsId: malicious X-Vbt-Ver is sanitized to 'unknown'", async () => {
    const payload = new Uint8Array([1, 2, 3]);
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      headers: { "X-Vbt-Ver": "../../../etc/passwd" },
      body: payload,
    });
    const res = await handlePutReport(req, ctx.env, NOW);
    expect(res.status).toBe(200);
    expect(ctx.r2.objects).toHaveLength(1);
    const obj = ctx.r2.objects[0]!;
    expect(obj.key.startsWith(`reports/${utcDateString(NOW)}/unknown/`)).toBe(
      true,
    );
    expect(obj.key).not.toContain("..");
    expect(obj.key.endsWith(".gz")).toBe(true);
  });

  it("records daily usage after a successful upload", async () => {
    const payload = new Uint8Array([1, 2, 3, 4, 5]);
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      headers: { "X-Vbt-Ver": "1.0.0" },
      body: payload,
    });
    await handlePutReport(req, ctx.env, NOW);
    const stored = await ctx.budgetDO.stub.storage.get(
      `budget:${dayKeyFor(NOW)}`,
    );
    expect(stored).not.toBeUndefined();
    expect(stored!.bytes).toBe(5);
    expect(stored!.requests).toBe(1);
  });

  it("falls back to 'unknown' caller ip when CF-Connecting-IP is absent", async () => {
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      body: new Uint8Array([1]),
    });
    await handlePutReport(req, ctx.env, NOW);
    expect(ctx.limiter.calls).toEqual([{ key: "unknown" }]);
  });

  it("atomicBudget: two concurrent requests cannot both pass a budget with room for only one", async () => {
    const ctx2 = makeEnv();
    // Seed the request counter one below MAX_DAILY_REQUESTS (4000), so
    // exactly one more request fits under the cap, not two.
    await ctx2.budgetDO.stub.seed(dayKeyFor(NOW), { bytes: 0, requests: 3999 });

    const makeReq = () =>
      new Request("https://example.com/v1/report", {
        method: "PUT",
        body: new Uint8Array([1, 2, 3]),
      });

    const [res1, res2] = await Promise.all([
      handlePutReport(makeReq(), ctx2.env, NOW),
      handlePutReport(makeReq(), ctx2.env, NOW),
    ]);

    const statuses = [res1.status, res2.status].sort();
    expect(statuses).toEqual([200, 503]);
    expect(ctx2.r2.objects).toHaveLength(1);
  });
});

describe("default export fetch wrapper", () => {
  it("returns 500 on internal error and does not leak details", async () => {
    const ctx = makeEnv();
    ctx.limiter.success = true;
    const original = ctx.r2.put.bind(ctx.r2);
    ctx.r2.put = async () => {
      throw new Error("synthetic r2 failure with secret stack info");
    };
    const req = new Request("https://example.com/v1/report", {
      method: "PUT",
      body: new Uint8Array([1, 2, 3]),
    });
    const res = await worker.fetch(req, ctx.env);
    expect(res.status).toBe(500);
    const text = await res.text();
    expect(text).not.toContain("synthetic");
    expect(text).not.toContain("secret stack");
    ctx.r2.put = original;
  });
});
