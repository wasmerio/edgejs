# ECO-355 — Framework tests for popular self-hosted apps

Status: Phase 1 done (js-dashy, js-etherpad, js-totaljs-cms green).
SQLite spike concluded **no-go** — Phase 2 as originally written is dead.
Current work: DB-backed apps via harness-provisioned embedded DB binaries
(see "Phase DB" below).
Linear: [ECO-355](https://linear.app/wasmer/issue/ECO-355/port-some-popular-self-hosted-apps-to-edgejsquickjs)
Owner: Arshia Ghafoori

## Goal

Port a set of popular self-hosted apps to run on EdgeJS+QuickJS and cover each
with a framework test, so we get continuous regression signal that real-world
Node backend apps boot and serve under the runtime.

Hard rule from the ticket: **find every blocker and fix it upstream — no
per-app AI workarounds / custom fixups.** The apps must work on a clean,
unpatched checkout.

Apps in scope (from the ticket, ranked by popularity):

| Rank | App | Type | Default storage |
| -- | -- | -- | -- |
| 1 | Uptime Kuma | Monitoring / status pages | SQLite |
| 2 | Ghost | Blog / publishing | MySQL (SQLite dev) |
| 3 | Umami | Web analytics | PostgreSQL |
| 4 | Directus | SQL CMS / API | SQLite (+ others) |
| 5 | Actual Budget | Personal finance | SQLite-style |
| 6 | Dashy | Homelab dashboard | Config / files |
| 7 | Etherpad | Collaborative editor | DirtyDB file (SQL configurable) |
| 8 | HedgeDoc | Collaborative notes | SQLite (+ others) |
| 9 | Firekylin | Blog platform | MySQL / SQLite |
| 10 | RSSMonster | RSS reader | MySQL |
| 11 | Total.js CMS | CMS | Filesystem DB |

## Background: the existing framework-test harness

These tests plug into the harness already in the repo. Key facts (full spec in
`plans/framework-integration-tests.md`):

- **Discovery is automatic.** `scripts/framework-test.js` scans the
  `wasmer-examples` submodule (separate repo `wasmerio/examples`) for top-level
  `js-*` directories containing a `package.json`. To add an app you commit a
  `js-<app>/` directory there, then bump the submodule pointer in this repo.
- **Multi-stage matrix.** Each app runs through Node baseline → EdgeJS/QuickJS
  native → QuickJS-WASIX (safe). A later stage only tests apps that passed the
  previous one.
- **Build on Node, run on Edge.** Builds always execute on host Node (native
  tooling / SWC). Edge stages run the app's production runtime script
  (`preview`/`serve`/`start`) under EdgeJS by swapping
  `node_modules/.bin/node` for the EdgeJS binary.
- **Assertions live in `routes.json`** beside each app: `path`, `method`,
  `body`, `headers`, `expect.status` (default `[200,304]`),
  `expect.contentType` (`html`/`json`/`any`), `expect.bodyContains[]`,
  `expect.bodyRegex[]`, plus per-route `stages` allowlist and `skipOnStatic`.
- **Run locally:** `make framework-test js-<app>` (single app) or
  `make framework-test-quickjs-native` / `...-quickjs-wasix` for the CI
  matrices. Per-runtime exclusions go in `FRAMEWORK_TEST_NODE_SKIP` /
  `FRAMEWORK_TEST_EDGE_SKIP` in the `Makefile`. Logs land in
  `.framework-test/logs/<app>.<stage>.{build,server}.log`.
- **Use `fail-`/`skip-` directory prefixes** in the examples repo for apps that
  are committed but not yet passing, so they're tracked without breaking CI.

## Why these apps are different from what the harness has tested

Every existing example is a **static-site or SSR frontend** (Next, Astro,
Gatsby, Svelte, Docusaurus). The ECO-355 apps are **stateful backend servers**.
They introduce three requirements the harness has never had to meet:

1. **A database.** The harness provisions none today.
2. **First-run setup** — migrations and an admin user — before any route is
   meaningful.
3. **Long-running server processes** under EdgeJS (not build-to-static).

### The decisive blocker: storage

- `node:sqlite` is **explicitly disabled** in EdgeJS — it sits in the
  `cannot_be_required` set at `src/builtin_catalog.cc:115`.
- Native addons (`better-sqlite3`, `@louislam/sqlite3`, `sqlite3`, the Prisma
  query engine) **cannot load under QuickJS/WASIX**.

So any app whose only storage path is SQLite-via-native-addon, or an external
DB server, is **blocked until storage is fixed in the runtime**. Fixing that is
the heart of this ticket, which is why it comes first.

## App triage

Working hypothesis (native-dep specifics verified per app during its phase):

| App | Storage | Native dep? | Tier |
| -- | -- | -- | -- |
| Dashy | Config/YAML files + tiny Express server | No | A — no-DB |
| Total.js CMS | Filesystem TextDB (NoSQL) | No | A — no-DB |
| Etherpad | DirtyDB (file-based JSON) | No (DirtyDB mode) | A — no-DB |
| Directus | SQLite | `better-sqlite3` | B — needs SQLite |
| HedgeDoc | SQLite (Sequelize) | `sqlite3` | B — needs SQLite |
| Uptime Kuma | SQLite | `@louislam/sqlite3` | B — needs SQLite |
| Actual Budget | SQLite | `better-sqlite3` + absurd-sql | B — needs SQLite |
| Firekylin | MySQL / SQLite (ThinkJS) | native sqlite | B — needs SQLite |
| RSSMonster | MySQL (Sequelize) | external MySQL | C — external DB |
| Umami | PostgreSQL (Prisma) | Prisma engine + Postgres | C — external DB |
| Ghost | MySQL (SQLite dev) | `better-sqlite3`, Ember admin build | C — external DB / heaviest |

## Execution order

We start with **Phase 1 (no-DB apps)** now. The SQLite decision (whether/how to
add it) is deferred and runs in parallel as a research spike; it only gates
Phase 2. Steps are sequential and independently-shippable — don't start one
until the previous is merged and green.

```
Phase 1: no-DB apps   →   Phase 2: SQLite apps   →   Phase 3: external-DB apps
   (start now)                  ↑
                          SQLite decision
                       (parallel spike, gates Phase 2)
```

---

### Phase 1 — Tier A: no-DB apps (Dashy, Total.js CMS, Etherpad) — START HERE

These need no native DB, so they exercise the **backend-server** path of the
harness without depending on any storage work. They de-risk the harness's
server-app support and deliver the first real self-hosted apps, while the SQLite
question is still open.

Per app (`js-dashy`, `js-totaljs-cms`, `js-etherpad`):
1. Add the app to the `wasmerio/examples` submodule, configured for its file
   storage (Total.js TextDB / Etherpad DirtyDB / Dashy YAML).
2. Add `routes.json` asserting a stable page (dashboard / pad / login) via
   `bodyContains`.
3. Get green on Node → EdgeJS-native → WASIX. File runtime blockers found
   along the way as upstream fixes.
4. Add any per-runtime exclusions to the Makefile skip-lists; README per app.

Likely harness extensions needed here (land in
`scripts/lib/framework-test-shared.js`, shared with later phases):
- A per-app **setup/seed hook** (e.g. a `prestart` step or a
  `framework-setup.json`) to run first-run setup deterministically before route
  checks.
- A **longer readiness timeout** — current `SERVER_READY_TIMEOUT_MS` is 45s.
- A **health-route convention** so assertions hit a deterministic page rather
  than a one-time setup wizard.

Exit criteria: all three apps pass the full matrix in CI; harness extensions
documented in `plans/framework-integration-tests.md`.

---

### SQLite decision — RESOLVED: no-go (2026-07)

We decided **not** to add SQLite support to the runtime. The original Phase 2
(SQLite apps) is cancelled in that form. However, all Tier B apps except one
can also run against MySQL or PostgreSQL, so they fold into the external-DB
phase below instead of being dropped.

### Phase DB — external-DB apps via embedded DB binaries (CURRENT)

Replaces the old Phase 2/Phase 3 split. The harness gains the ability to
provision an **ephemeral real database per app** using embedded-binary npm
packages — no Docker (framework tests run on `macos-latest` CI runners, which
have no Docker daemon, and must also run locally via `make framework-test`):

- PostgreSQL: `embedded-postgres` (zonky.io binaries, mac/linux/windows)
- MySQL: `mysql-memory-server`

Design constraints:
- DB is spawned as a plain child process by the harness setup hook: unique
  port, temp datadir, connection info injected via `makeProjectEnv` env vars,
  torn down after the app's run (including on failure/interrupt).
- Works identically across node/edge-native/wasix stages. WASIX reaches the
  DB over host loopback (`wasmer run --net` supports localhost — confirmed,
  no test needed).
- Binary downloads are cacheable in CI (`actions/cache`).

App triage (researched 2026-07-03, per-app package.json + docs verified):

| App | DB | Verdict |
| -- | -- | -- |
| HedgeDoc 1.x | Postgres or MySQL (`CMD_DB_URL`) | **viable — first app**; auto-migrations on boot, unauth `/`, `/status`, `/_health` |
| RSSMonster | MySQL only | viable; `sequelize db:migrate && db:seed:all`, `/api/health` |
| Uptime Kuma 2.x | MariaDB (env-driven) | viable with friction (admin setup is a web wizard; sqlite driver provably not loaded in mariadb mode) |
| Firekylin | MySQL or Postgres | viable-ish (web install wizard needs config pre-seeding) |
| Umami | Postgres | **blocked**: Prisma 7 query compiler is a WASM module (no WebAssembly in QuickJS); Prisma 6 = native engine |
| Ghost | MySQL 8 | **blocked**: `sharp` is a hard dep (native); heaviest build; Node ^22.18 pin |
| Directus | any | **blocked**: `sharp` + `isolated-vm` + `argon2` hard deps (native) |
| Actual Budget | sqlite-style only | dropped (the one app with no MySQL/Postgres path) |

Start order: HedgeDoc (Postgres, **done**) → RSSMonster (MySQL, **done** —
added the mysql provider via mysql-memory-server plus a `database.setup`
hook for sequelize migrations/seeds) → Uptime Kuma (**done on native; skipped
on the WASIX edge stage** — playwright-core's registry throws
`Unsupported platform: wasi` at require time, and the process.platform='linux'
parity change it originally drove was not merged; also drove the
expected-status readiness probes) → Firekylin (**done — full matrix including WASIX**; drove three runtime
fixes: libuv-wasix IPC reads (fork/process.send channels now work under
WASIX), SO_REUSEPORT enablement in libuv-wasix, and a WASIX cluster
reuseport scheduling strategy in edge's lib/internal/cluster — workers bind
their own SO_REUSEPORT listeners since handles cannot be passed between
processes; the host kernel balances connections. Also drove the MySQL
provider's mysql_native_password user + 8.0.x pin for legacy `mysql` 2.x
drivers). All four planned DB apps are landed and green on the full matrix.

Note: native addon loading is now deliberately disabled on the native edge
binaries (process.dlopen throws catchable ERR_DLOPEN_FAILED) so native and
WASIX expose the same functionality — apps hard-requiring native addons
fail identically everywhere.

<details>
<summary>Original (obsolete) SQLite spike text, kept for history</summary>

### SQLite decision (parallel spike — gates Phase 2, not Phase 1)

Run this as a research spike alongside Phase 1. **Decision pending: do we add
SQLite at all, and if so, how?** Five Tier B apps cannot run without it, so the
outcome decides whether Phase 2 happens.

Options to evaluate, in rough order of preference:
1. **Enable `node:sqlite`** — it is bundled but blocklisted at
   `src/builtin_catalog.cc:115`; remove it from `cannot_be_required` and make the
   builtin actually functional under both the native QuickJS and WASIX backends.
2. **Ship a WASM SQLite** (e.g. `wa-sqlite` / sqlite compiled to WASM) and
   expose it so app ORMs can reach it.
3. **Pure-JS driver shim** that Knex/Sequelize can target.

Whichever path: it must be a runtime/upstream capability, **not** a per-app
patch, and must persist to the filesystem under WASIX (`wasmer run --net`, app
dir mapped to `/app`). Validate with a focused runtime test (under `test/`)
proving open / migrate / insert / query / reopen-and-read-back on native QuickJS
and WASIX.

Exit of the spike = a go/no-go and, if go, a chosen path + a working storage
smoke test. Only then does Phase 2 start.

---

### Phase 2 — Tier B: SQLite apps (Directus, HedgeDoc, Uptime Kuma, Actual Budget, Firekylin)

**Gated on the SQLite decision above.** Only proceed if that spike lands "go".
Bring on one app at a time — each is a separate, mergeable change.

Per app:
1. Add `js-<app>` to the examples submodule, storage pinned to the SQLite path
   chosen in the spike.
2. Add a **seed/migration step** (admin user + schema) via the Phase 1 setup
   hook so routes are deterministic.
3. Add `routes.json` (login / dashboard / health `bodyContains`).
4. Green on Node → EdgeJS-native → WASIX; fix blockers upstream.
5. Skip-lists + README as needed.

Suggested intra-phase order (lightest first): Directus → HedgeDoc →
Uptime Kuma → Actual Budget → Firekylin. Adjust as blockers surface.

Exit criteria: each app passes the matrix (or is parked under a `fail-` prefix
with a tracked upstream blocker).

---

### Phase 3 — Tier C: external-DB / heaviest apps (RSSMonster, Umami, Ghost)

These need an external DB server (MySQL/Postgres) or are very heavy (Ghost is a
monorepo with an Ember admin build).

Open decision for the start of this phase: should the harness gain a capability
to **orchestrate a DB service** (spin up MySQL/Postgres for the test), or do
these apps stay out of scope for ECO-355? Resolve before committing app work.

If in scope, per app:
1. Stand up the required DB (new harness capability or CI service container).
2. Add `js-<app>` + seed + `routes.json`.
3. Green on the matrix; fix blockers upstream.

Ghost is the last/optional item given the Ember admin build and MySQL
dependency.

</details>

## Cross-cutting conventions

- Apps live in the `wasmerio/examples` submodule; each change there is paired
  with a submodule-pointer bump in `edgejs`.
- `fail-`/`skip-` prefixes track committed-but-not-passing apps without breaking
  CI.
- Each app ships a README mirroring the examples-repo convention.
- CI already runs `make framework-test-quickjs-wasix`; newly committed apps are
  discovered automatically, gated by the Makefile skip-lists.
- Every blocker is fixed in the runtime / upstream project — never patched per
  app in the example.

## Open questions

- ~~SQLite go/no-go~~ — resolved: no-go; DB apps run against embedded
  MySQL/Postgres instead (see Phase DB).
- ~~Phase 3 DB orchestration~~ — resolved: harness provisions embedded DB
  binaries per app (no Docker).
- Which single former-Tier-B app has no MySQL/Postgres path (and is therefore
  dropped)? Confirm during Phase DB triage — likely Actual Budget.
