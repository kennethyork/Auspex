#include "auspex/skills.hpp"

namespace auspex {

// Project-type starters, ported from ollamadev-qt's crew team library.
//
// A SECOND AXIS, not more of the same. starter_skills() is matched against the
// TASK and carries capabilities -- how to write a test, how not to leak a
// credential. These are matched against the FOCUS and carry what a KIND OF
// PROJECT needs: a SaaS scopes every query by tenant, an e-commerce store keeps
// money in integer minor units, a PWA versions its caches. A crew told it is
// working on a shop should know the second thing before it writes the first line.
//
// Kept in their own file because they are data, and 34 entries of data in
// skills.cpp would bury the code that uses them.
//
// TRIGGERS ARE ollamadev's, with six widenings. Theirs are narrow on purpose, but
// a few were narrow enough to never fire from a focus somebody would actually
// write: the data-pipeline PACK says "a data-processing pipeline" and the
// data-pipeline STARTER triggered only on "data pipeline", so the two halves of
// the same preset never met. Each addition is a phrasing with no realistic false
// match -- "game" cannot fire on "gamification", which does not contain it.
const std::vector<SkillSpec>& project_starters() {
    static const std::vector<SkillSpec> kLibrary{
        {"website",
         {"website", "marketing site"},
         "Website (static / marketing / content) — project starter.",
         "# website\\n"
         "- Semantic, accessible markup — landmarks, one <h1>, real buttons/links.\\n"
         "- Responsive, mobile-first; nothing overflows on small screens.\\n"
         "- SEO basics: unique title/description, Open Graph, sitemap.\\n"
         "- Fast load: optimize and lazy-load images, keep JS minimal.\\n"},
        {"landing-page",
         {"landing page"},
         "High-converting landing page — project starter.",
         "# landing-page\\n"
         "- One clear hero and call-to-action above the fold.\\n"
         "- Responsive and fast; remove distractions from the goal.\\n"
         "- SEO/meta and Open Graph for shareable previews.\\n"
         "- A working contact/signup form with validation and feedback.\\n"},
        {"web-app",
         {"web app", "single-page app"},
         "Web application (SPA or full-stack) — project starter.",
         "# web-app\\n"
         "- Clear component structure with predictable, minimal state.\\n"
         "- Routing and auth wired; protect private routes.\\n"
         "- Handle loading / empty / error states for every async view.\\n"
         "- Integrate APIs defensively; add tests for core flows.\\n"},
        {"saas",
         {"saas"},
         "SaaS product (multi-tenant) — project starter.",
         "# saas\\n"
         "- Scope EVERY query by tenant id; deny by default.\\n"
         "- Secure auth and sessions; least-privilege roles.\\n"
         "- Reliable billing/subscription logic with idempotent webhooks.\\n"
         "- Per-tenant limits and an audit log for sensitive actions.\\n"},
        {"ecommerce",
         {"e-commerce", "ecommerce"},
         "E-commerce (catalog / cart / checkout) — project starter.",
         "# ecommerce\\n"
         "- Store money as integer minor units, never float; track currency.\\n"
         "- Compute totals and tax server-side from trusted prices.\\n"
         "- Make payment and webhook handling idempotent; verify signatures.\\n"
         "- Guard inventory against oversell with atomic updates.\\n"},
        {"admin-dashboard",
         {"admin dashboard", "internal tool"},
         "Admin dashboard / internal tool — project starter.",
         "# admin-dashboard\\n"
         "- Accurate data with server-side pagination, sort, and filter.\\n"
         "- Role-based access checked on every action.\\n"
         "- Clear tables, forms, and charts; confirm destructive operations.\\n"
         "- Optimistic UI with rollback on failure.\\n"},
        {"blog-cms",
         {"blog", "cms"},
         "Blog or CMS — project starter.",
         "# blog-cms\\n"
         "- Clear content models and a stable slug/permalink scheme.\\n"
         "- Sanitize and safely render user/markdown content.\\n"
         "- SEO and RSS; draft/publish workflow.\\n"
         "- Cache rendered content; invalidate on edit.\\n"},
        {"docs-site",
         {"docs site", "documentation site"},
         "Documentation site — project starter.",
         "# docs-site\\n"
         "- Clear navigation and working search.\\n"
         "- Runnable, tested code samples; keep them accurate.\\n"
         "- Logical structure and consistent terminology.\\n"
         "- Versioned docs and a fast static build.\\n"},
        {"forum-community",
         {"forum", "community"},
         "Forum / community app — project starter.",
         "# forum-community\\n"
         "- Data integrity for threads, posts, and users.\\n"
         "- Moderation tools plus spam/abuse handling.\\n"
         "- Rate-limit posting; paginate and index for scale.\\n"
         "- Notifications without N+1 query blowups.\\n"},
        {"pwa-app",
         {"progressive web app"},
         "Progressive Web App — project starter.",
         "# pwa-app\\n"
         "- Ship a manifest; make it installable.\\n"
         "- Service worker: precache the shell, runtime-cache data.\\n"
         "- Offline fallback and graceful degradation.\\n"
         "- Version caches and clean old ones; serve over HTTPS.\\n"},
        {"mobile",
         {"mobile app", "ios app", "android app"},
         "Mobile app (iOS / Android / RN / Flutter) — project starter.",
         "# mobile\\n"
         "- Handle the app lifecycle and restore state.\\n"
         "- Keep the UI thread free; do heavy work off it.\\n"
         "- Follow each platform navigation and UX conventions.\\n"
         "- Handle offline and flaky networks; request permissions just-in-time.\\n"},
        {"desktop",
         {"desktop app"},
         "Desktop app (Electron / Tauri / Qt / GTK) — project starter.",
         "# desktop\\n"
         "- Keep heavy work off the UI thread; stay responsive.\\n"
         "- Sandbox any renderer and validate IPC messages.\\n"
         "- Handle multi-window and OS lifecycle cleanly.\\n"
         "- Store user data in OS-appropriate paths; test packaging.\\n"},
        {"rest-api",
         {"rest api", "backend service"},
         "REST API / backend service — project starter.",
         "# rest-api\\n"
         "- Noun resources, correct verbs and status codes.\\n"
         "- Validate every input at the boundary.\\n"
         "- Consistent error shape; paginate list endpoints.\\n"
         "- Authorize per request; add tests.\\n"},
        {"graphql",
         {"graphql api"},
         "GraphQL API — project starter.",
         "# graphql\\n"
         "- Design the schema around client needs.\\n"
         "- Use dataloader/batching to kill N+1.\\n"
         "- Cursor-based pagination instead of unbounded lists.\\n"
         "- Enforce auth in resolvers; limit query depth/complexity.\\n"},
        {"realtime",
         {"realtime service", "websocket service"},
         "Realtime / WebSocket service — project starter.",
         "# realtime\\n"
         "- Handle the full connection lifecycle with reconnect backoff.\\n"
         "- Authenticate on connect and per room/channel join.\\n"
         "- Apply backpressure when a client cannot keep up.\\n"
         "- Make handlers idempotent; clean up on disconnect.\\n"},
        {"serverless-fn",
         {"serverless function", "cloud function"},
         "Serverless functions — project starter.",
         "# serverless-fn\\n"
         "- Stay stateless; never persist to local disk/memory.\\n"
         "- Minimize cold starts; init clients outside the handler.\\n"
         "- Secrets from env/manager; least-privilege IAM per function.\\n"
         "- Idempotent on duplicate events; return structured errors.\\n"},
        {"microservice",
         {"microservice"},
         "Microservice — project starter.",
         "# microservice\\n"
         "- Single responsibility and a clear API contract.\\n"
         "- Health/readiness checks and meaningful logs.\\n"
         "- Observability: logs, metrics, traces with a correlation id.\\n"
         "- Resilient calls: timeouts, retries, circuit breaking.\\n"},
        {"database",
         {"database schema", "migrations"},
         "Database / schema work — project starter.",
         "# database\\n"
         "- Normalize first; index what you filter, join, and sort on.\\n"
         "- Enforce integrity: foreign keys, NOT NULL, unique, checks.\\n"
         "- Reversible, safe-on-live migrations: add nullable, backfill,\\n"
         "  constrain.\\n"
         "- Parameterize queries; back up before destructive changes.\\n"},
        {"data-pipeline",
         {"data pipeline", "data-processing pipeline", "data processing", "etl job"},
         "Data pipeline / ETL — project starter.",
         "# data-pipeline\\n"
         "- Make every stage idempotent so a re-run cannot double-write.\\n"
         "- Validate incoming data; quarantine bad records.\\n"
         "- Checkpoint progress so a failed run resumes.\\n"
         "- Deterministic, testable transforms; log row counts in/out.\\n"},
        {"data-ml-project",
         {"data science project", "machine learning project"},
         "Data / ML project — project starter.",
         "# data-ml-project\\n"
         "- Set and record seeds; pin library versions.\\n"
         "- Validate data; never leak test into train.\\n"
         "- Keep preprocessing in re-runnable code, not manual steps.\\n"
         "- Track metrics and the params that produced them.\\n"},
        {"ai-app",
         {"ai app", "llm application"},
         "AI / LLM app — project starter.",
         "# ai-app\\n"
         "- Budget tokens; truncate or summarize before the limit.\\n"
         "- Stream output; handle partial and mid-stream errors.\\n"
         "- Parse model output defensively (schema/JSON mode).\\n"
         "- Cache deterministic calls; guard privileged actions.\\n"},
        {"game",
         {"game project", "game"},
         "Game — project starter.",
         "# game\\n"
         "- Separate fixed-timestep update from render; scale by delta time.\\n"
         "- Pool/reuse objects to avoid per-frame GC spikes.\\n"
         "- Keep per-frame work bounded; profile before optimizing.\\n"
         "- Load assets async; never stall the loop on I/O.\\n"},
        {"cli",
         {"cli tool", "command-line tool", "command line tool"},
         "CLI tool — project starter.",
         "# cli\\n"
         "- Provide --help and a clear usage line.\\n"
         "- Exit 0 on success; errors to stderr, data to stdout.\\n"
         "- Offer --json for machine consumption.\\n"
         "- Validate args early; confirm destructive actions unless --force.\\n"},
        {"library",
         {"library", "sdk package"},
         "Library / SDK / package — project starter.",
         "# library\\n"
         "- Keep the public surface small and intentional.\\n"
         "- Follow semantic versioning; document every export with an example.\\n"
         "- Fail loudly on misuse; validate inputs at the boundary.\\n"
         "- No global mutable state or side effects on import.\\n"},
        {"browser-ext",
         {"browser extension", "chrome extension"},
         "Browser extension (Manifest V3) — project starter.",
         "# browser-ext\\n"
         "- Request the minimum permissions; justify each.\\n"
         "- Keep the service worker lean and event-driven.\\n"
         "- Validate messages between content and background scripts.\\n"
         "- Bundle everything; no remote code.\\n"},
        {"vscode-ext",
         {"vs code extension", "vscode extension"},
         "VS Code extension — project starter.",
         "# vscode-ext\\n"
         "- Declare contribution points and activation events; keep activation\\n"
         "  lean.\\n"
         "- Use the extension API and dispose resources you create.\\n"
         "- Handle workspace and multi-root cases.\\n"
         "- Test in the Extension Development Host.\\n"},
        {"plugin",
         {"plugin for", "plugin"},
         "Plugin (WordPress / Figma / Obsidian / …) — project starter.",
         "# plugin\\n"
         "- Follow the host plugin API, hooks, and lifecycle.\\n"
         "- Do not pollute global or host state.\\n"
         "- Validate inputs coming from the host.\\n"
         "- Clean packaging and versioning; fail gracefully on host API changes.\\n"},
        {"chatbot",
         {"chat bot", "chatbot"},
         "Chat bot (Discord / Slack / Telegram) — project starter.",
         "# chatbot\\n"
         "- Keep the platform token secret (env, not code).\\n"
         "- Respect rate limits; queue and back off.\\n"
         "- Acknowledge events fast; do slow work async.\\n"
         "- Validate and authorize commands; stay idempotent on reconnect.\\n"},
        {"automation",
         {"automation script"},
         "Automation / script — project starter.",
         "# automation\\n"
         "- Robust I/O and error handling.\\n"
         "- Idempotent so it is safe to re-run.\\n"
         "- Structured logging; clear exit codes.\\n"
         "- Handle credentials safely; never hard-code secrets.\\n"},
        {"devops",
         {"devops", "infrastructure"},
         "DevOps / infra — project starter.",
         "# devops\\n"
         "- Make everything idempotent and declarative.\\n"
         "- Secrets from a store; least privilege everywhere.\\n"
         "- Pin versions/images; fail-fast, cache-friendly stages.\\n"
         "- Plan/diff before apply; keep changes reversible.\\n"},
        {"ci-cd",
         {"ci/cd pipeline", "ci cd"},
         "CI/CD pipeline — project starter.",
         "# ci-cd\\n"
         "- Stage build, test, then deploy.\\n"
         "- Cache dependencies; fail fast.\\n"
         "- Secrets via the CI store; never print them to logs.\\n"
         "- Reproducible and pinned; gate production behind review.\\n"},
        {"embedded-iot",
         {"embedded", "firmware", "iot device"},
         "Embedded / IoT / firmware — project starter.",
         "# embedded-iot\\n"
         "- Avoid dynamic allocation in hot/interrupt paths; bound buffers.\\n"
         "- Keep ISRs tiny; defer work to the main loop.\\n"
         "- Mind timing and watchdogs; do not block on I/O.\\n"
         "- Be explicit about integer widths and endianness.\\n"},
        {"web3",
         {"smart contract", "web3 dapp"},
         "Smart contract / web3 dApp — project starter.",
         "# web3\\n"
         "- Checks-effects-interactions order; guard against reentrancy.\\n"
         "- Use safe math; watch overflow and rounding.\\n"
         "- Minimize and audit external calls; emit events on state change.\\n"
         "- Write adversarial tests before deploy.\\n"},
        {"security-project",
         {"security hardening project"},
         "Security hardening project — project starter.",
         "# security-project\\n"
         "- Validate/escape input; parameterize SQL; escape output.\\n"
         "- Never commit secrets; scan the diff before committing.\\n"
         "- Authorize every sensitive action; deny by default.\\n"
         "- Avoid shelling out with user input; keep dependencies patched.\\n"},
    };
    return kLibrary;
}

}  // namespace auspex
