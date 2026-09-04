// Pure, storage-agnostic budget logic shared by the real DailyBudgetCounter
// Durable Object (src/index.ts) and by tests, which pass a fake in-memory
// storage implementing the same two-method shape.

export interface BudgetState {
  bytes: number;
  requests: number;
}

export interface BudgetStorageLike {
  get(key: string): Promise<BudgetState | undefined>;
  put(key: string, value: BudgetState): Promise<void>;
}

export interface BudgetCheckResult {
  allowed: boolean;
  bytesToday: number;
  requestsToday: number;
}

// Atomic check-and-increment against a single storage key. Safe under
// concurrency ONLY because the caller (a Durable Object) guarantees this
// function's get() and put() are never interleaved with another invocation
// of this same function on the same object instance — Durable Objects defer
// starting a new incoming request until any in-flight storage operation on
// the object completes (the "input gate"), and this function performs
// exactly one get() and one put() with no other await between them, so no
// second invocation can observe stale state mid-decision. A KV-backed
// read-modify-write has no equivalent guarantee, which is the bug this
// replaces (Wave 7 BLOCKER 2).
//
// The budget is counted here, at check time, not after the caller's
// subsequent R2 put succeeds. An atomic check-and-increment cannot defer
// the increment across an intervening I/O call without reopening the same
// race window this exists to close — so a report that fails to reach R2
// after passing this check still counts against the day's budget. That is
// an intentional fail-closed tradeoff, not a regression.
export async function checkAndIncrementBudget(
  storage: BudgetStorageLike,
  day: string,
  incomingBytes: number,
  capBytes: number,
  capRequests: number,
): Promise<BudgetCheckResult> {
  const key = `budget:${day}`;
  const state = (await storage.get(key)) ?? { bytes: 0, requests: 0 };
  const projectedBytes = state.bytes + incomingBytes;
  const projectedRequests = state.requests + 1;
  const allowed =
    projectedBytes <= capBytes && projectedRequests <= capRequests;
  if (!allowed) {
    return {
      allowed: false,
      bytesToday: state.bytes,
      requestsToday: state.requests,
    };
  }
  const next: BudgetState = {
    bytes: projectedBytes,
    requests: projectedRequests,
  };
  await storage.put(key, next);
  return { allowed: true, bytesToday: next.bytes, requestsToday: next.requests };
}
