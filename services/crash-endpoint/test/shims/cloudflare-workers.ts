// Test-only shim for the "cloudflare:workers" virtual module, which only
// exists inside the real Workers runtime. Plain Vitest cannot resolve it,
// so vitest.config.ts aliases the bare specifier to this file. Faithfully
// mirrors the documented constructor contract of the real DurableObject
// base class (ctx stored as this.ctx, env stored as this.env). This has
// no effect on the deployed Worker: wrangler's bundler resolves
// "cloudflare:*" specifiers against the real workerd runtime, never
// against this file.
export class DurableObject<Env = unknown> {
    ctx: DurableObjectState;
    env: Env;
    constructor(ctx: DurableObjectState, env: Env) {
        this.ctx = ctx;
        this.env = env;
    }
}