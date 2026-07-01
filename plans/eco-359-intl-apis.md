# ECO-359: Implement missing Intl APIs in EdgeJS QuickJS (ICU-backed)

Status: **PLANNED** (2026-07-01). Assignee: Arshia. Project: Productize EdgeJS+QuickJS.
ECO-381 (Etherpad `Intl.ListFormat` blocker) is closed as a **duplicate** of this issue.

## Decisions (locked)
- **Full ECMA-402 surface**, not a minimal unblocker: all 9 constructors + `getCanonicalLocales`.
- **ICU-backed**, in C++/N-API. The correct backend (`edge_icu_i18n`) is *already compiled and
  linked* into both providers — we call it instead of hand-rolling.
- **Wire real ICU data into native builds too**, not only WASIX (accept the ~11 MB size hit) so
  `build-edge-quickjs-cli` can run the conformance probe locally.

## Root cause / how an incomplete Intl slipped through
Intl falls through the crack between the three layers of this stack:
1. **Node's JS sources (which we mirror faithfully) do not implement Intl.** `lib/internal/freeze_intrinsics.js:365-370`
   *references* `Intl.Collator/NumberFormat/PluralRules/RelativeTimeFormat/ListFormat`, but nothing under
   `lib/` implements them. In Node, Intl is V8+ICU **native** code, so mirroring the JS sources perfectly
   still yields no Intl.
2. **The engine provides none.** Engine is **quickjs-ng 0.14.0** (`napi/quickjs/deps/quickjs/quickjs.h:1496`).
   Exhaustive grep of the vendored engine for `Intl`/`CONFIG_INTL`/`JS_AddIntrinsicIntl`/`DateTimeFormat`
   returns nothing. Unlike V8, quickjs-ng ships no ECMA-402 layer to inherit. There is no disabled flag to
   flip — the feature simply does not exist in the engine.
3. **So EdgeJS hand-wrote the entire Intl surface** as a native N-API shim — and only ever implemented
   `DateTimeFormat`. `src/edge_intl_fallback.cc` installs a single constructor, locale hardcoded to `en-US`
   (`:10`), formatting done with `Date` methods + manual `Pad2` + hand-rolled AM/PM (`:241-291`), **no ICU**.
   It is documented as deliberately minimal — "only meant to unblock framework bootstrap formatting, not to
   claim full ECMA-402 compatibility" (`plans/quickjs-wasm/development/dev_001_pr_cleanup_containment/003_intl_fallback_module.md`;
   origin `.../troubleshooting/astro-ssr/004_missing_intl.md`).

It went unnoticed because it lives at the **engine boundary** — the one seam the Node-source-mirroring
contract explicitly does not cover — so no source diff would ever flag the gap. It only surfaces when an app
calls a missing constructor at runtime (Ghost → `NumberFormat`, Etherpad → `ListFormat`). The irony: **full
ICU is already in the binary and goes unused** while we hand-roll fake en-US dates.

## Current state (concrete)
- Intl shim: `src/edge_intl_fallback.cc` / `.h`. Entry `EdgeInstallMinimalIntlFallback` (`:391`), installs
  only `DateTimeFormat` (`InstallDateTimeFormat :355`). Called once from `src/edge_runtime.cc:2631`
  (guarded by `HasUsableDateTimeFormat`). Compiled via `src/CMakeLists.txt:7`.
- ICU build: `cmake/EdgeICU.cmake` — `edge_icu_stubdata` / `edge_icu_common` / `edge_icu_i18n` (globs
  `deps/icu-small/source/{stubdata,common,i18n}`), ICU **78** (Node's full-icu source subset). Exported as
  `EDGE_ICU_LINK_LIBS`, applied to *both* providers via `edge_apply_runtime_compile_surface`
  (`CMakeLists.txt:205,211`). `EDGE_HAS_ICU=1`, `EDGE_HAS_SMALL_ICU=1` (`CMakeLists.txt:227-228`).
- **Data split:** native links `stubdata.cpp` (empty image, `count=0`). WASIX
  (`wasix/cmake/icu_wasix.cmake`) embeds the real `deps/icu-small/source/data/in/icudt78l.dat.bz2`
  (~11.3 MB, ICU 78 LE) via `wasix/cmake/embed_binary.py` as symbol `ubi_icudt78l_dat` (bz2) into
  `edge_icu_embedded_data` → `edge_icu_common`.
- Current ICU consumers are **charset conversion** (`src/internal_binding/binding_icu.cc`, `ucnv_*`) and
  version reporting (`src/edge_process.cc:38`) — **not** Intl.

### ⚠ Linchpin (CONFIRMED 2026-07-01): ICU data is linked but never activated
Verified by static analysis, not just suspected:
- `wasix/cmake/embed_binary.py` **decompresses the bz2 at build time** (`bz2.decompress`) and emits the raw
  uncompressed ICU data as `ubi_icudt78l_dat[]` + `ubi_icudt78l_dat_len`. So the symbol is ready-to-use ICU
  data; no runtime decompress is needed.
- The **only** `udata_setCommonData` call in the whole tree is **V8's own** (`deps/v8/src/init/icu_util.cc:93`,
  bundled-v8 path). There is **no** `udata_setCommonData`/`udata_setAppData`/`u_setDataDirectory` anywhere in
  EdgeJS `src/`, `wasix/`, or the quickjs provider, and **nothing references `ubi_icudt78l_dat`** outside its
  generated definition.
- Therefore on WASIX, `edge_icu_common` links *both* `stubdata.cpp` (which defines ICU's real entry point
  `icudt78_dat` = empty) *and* the real blob — and since nobody calls `udata_setCommonData(ubi_icudt78l_dat)`,
  **ICU uses the empty stubdata; the 11.3 MB blob is dead-linked.** Even the shipping WASIX target has
  data-less ICU today. Native links only stubdata → also empty. This is exactly why `DateTimeFormat` had to be
  hand-rolled instead of calling the linked ICU.

**Fix (Phase 0):** call `udata_setCommonData(ubi_icudt78l_dat, &err)` once, early in bootstrap (mirroring
`deps/v8/src/init/icu_util.cc:93`), and make the embedded-data symbol available on **both** targets (native
currently omits it). Then ICU returns real locale data instead of `U_MISSING_RESOURCE_ERROR`; every
constructor below is mechanical after that.

## Approach
Replace the hand-rolled shim with a real ICU-backed N-API Intl module (`src/edge_intl.{cc,h}`, retiring
`edge_intl_fallback.*`). One C++ class per constructor wrapping the ICU C API; conceptually mirrors Node's
`src/node_intl`/`intl` layer but expressed in N-API so it rides the QuickJS provider. Each constructor:
resolves locale + options once (store on the instance), exposes prototype methods, and a static
`supportedLocalesOf`.

### Phase 0 — ICU data activation ✅ DONE (2026-07-01, native verified)
Implemented: `cmake/embed_binary.py` (moved from `wasix/`, now shared) embeds the real ICU blob as
`ubi_icudt78l_dat`; `cmake/EdgeICU.cmake` native branch builds `edge_icu_embedded_data` and links it into
`edge_icu_common`; `src/edge_icu_data.cc` calls `udata_setCommonData(ubi_icudt78l_dat)` once at bootstrap
(`edge_runtime.cc`, before Intl install), non-fatal on error. Verified on `build-edge-quickjs-cli`: clean
startup (no activation warning) and data-backed ICU converters now work —
`new TextDecoder('shift_jis').decode([0x82,0xa0])` → "あ", `gbk` [0xc4,0xe3] → "你" (both fail on empty
stubdata). WASIX build not yet re-verified (only the embed_binary.py path reference changed there).

### Phase 0 (original notes) — ICU data activation (BLOCKER for all else)
- Verify current WASIX behavior: call `uloc_getDefault`/`udat_open` from a probe; confirm whether data is live.
- If not registered, implement activation once, shared by both targets: decompress the embedded bz2 at
  runtime → `udata_setCommonData(data, &err)` before first Intl use (during runtime bootstrap, near
  `edge_runtime.cc:2631`). Alternative: switch the embed to an uncompressed `genccode` object exposing the
  real `icudt78_dat` entry point + `U_ICUDATAENTRY_IN_COMMON` (no runtime decompress). Prefer whichever the
  WASIX `edgejs.wasm` size budget tolerates; decompress-at-boot keeps the artifact small.
- **Native:** stop linking `stubdata.cpp` for the QuickJS target; link the same real-data path (reuse
  `embed_binary.py` + activation). Gate behind the provider so `bundled-v8` is untouched.
- Exit criteria: `new Intl.DateTimeFormat('en-GB').format(new Date())` via ICU returns a locale-correct
  string on both `build-edge-quickjs-cli` (native) and the WASIX artifact; no `U_MISSING_RESOURCE_ERROR`.

### Phase 1 — ICU-wrapping harness + retire the stub
**Progress (2026-07-01):** Harness landed in `src/edge_intl.{cc,h}` (`EdgeInstallIntl`, wired in
`edge_runtime.cc` right after the fallback, non-fatal). Shared helpers done: `ResolveIcuLocale`
(BCP-47→ICU via `uloc_forLanguageTag`), `IcuLocaleToBcp47`, `ToUChars`/`FromUChars`, `SupportedLocalesOf`
(generic; currently lenient — returns any parseable tag, does not yet verify data availability),
`ThrowRange`/`ThrowType`, `InstallConstructor`. **`Intl.ListFormat` implemented + verified** (ICU
`ulistfmt_*`): `format` (en `"a, b, and c"`, de `"a, b und c"`), `resolvedOptions`, static
`supportedLocalesOf`, RangeError on bad type/style, TypeError without `new`. The stub is NOT retired yet —
the new module currently *adds* to the fallback's Intl object; DateTimeFormat is folded in and the fallback
deleted at the end of Phase 2.

**`Intl.NumberFormat` implemented + verified** (ICU `unumf_*` modern formatter + concise skeletons): `format`,
`resolvedOptions`, static `supportedLocalesOf`; supports style decimal/currency/percent(scale/100)/unit,
`currency`, `useGrouping`, `min/maximumFractionDigits`. Verified: USD `$12.30` (issue acceptance), de-DE EUR
`1.234,50 €`, grouping `1,234,567.891`, percent `42%`, TypeError when currency missing. Not yet done for
NumberFormat: `formatToParts`, `currencyDisplay`/`notation`/`signDisplay`, unit-name mapping (ECMA-402
`kilometer` → ICU `length-kilometer`) — deferred to the completeness pass.

**Phase 2 COMPLETE (2026-07-01).** Full ECMA-402 surface implemented, ICU-backed, verified on
`build-edge-quickjs-cli`; the hand-rolled fallback is deleted. All 9 constructors + `getCanonicalLocales`:
- `ListFormat` (ulistfmt), `NumberFormat` (unumf), `DateTimeFormat` (udat+udatpg+ucal, incl. formatToParts
  + real timeZone), `PluralRules` (uplrules), `Collator` (ucol), `RelativeTimeFormat` (ureldatefmt),
  `DisplayNames` (uldn), `Locale` (uloc, accessors + maximize/minimize), `Segmenter` (ubrk, iterable
  Segments + containing), `getCanonicalLocales`.
- `Collator#compare` and `{Number,DateTime}Format#format` are bound to the instance at construction
  (`BindOwnMethod`) so they work detached (`arr.sort(c.compare)`, `arr.map(nf.format)`).
- Provider-safe: `InstallConstructor` skips any constructor already present, so the V8 provider's native
  Intl is left untouched; on QuickJS the whole surface is installed.
- **All 5 issue acceptance criteria pass** (verified via probe).

**Phase 2 polish DONE (2026-07-01):** `formatToParts` now implemented for NumberFormat (innermost-field
nesting handled: `-$1,234.50` → minusSign/currency/integer/group/integer/decimal/fraction) and ListFormat
(element/literal), alongside DateTimeFormat. `supportedLocalesOf` now filters to locales ICU actually has
data for (language-level lookup via `uloc_getAvailable`), so junk tags are dropped. Also fixed a latent
pointer-invalidation bug (SSO u16string `.data()` dangling on vector realloc) in the ListFormat element
arrays. 14-check regression sweep + all 5 acceptance criteria pass on native.

**Deeper coverage DONE (2026-07-01):**
- NumberFormat: `notation` (scientific/engineering/compact + `compactDisplay`), `signDisplay`
  (auto/never/always/exceptZero/negative), `currencyDisplay` (symbol/narrowSymbol/code/name),
  `currencySign` (accounting), `unit`/`unitDisplay` (CLDR unit ids like `kilometer-per-hour`),
  `minimum/maximumSignificantDigits`, `minimumIntegerDigits`; all surfaced in resolvedOptions.
  Verified: `1.2M`, `1.2 million`, `1.23456E5`, `+42`, `($5.00)`, `EUR 5.00`, `50 km/h`, `5 megabytes`,
  `0005`.
- DisplayNames: `currency` (ucurr_getName long/symbol) and `calendar` (uldn_keyValueDisplayName) types.
- Locale: Unicode `-u-` extension getters — `calendar`, `numberingSystem`, `hourCycle`, `collation`,
  `caseFirst`, `numeric` (bool); fixed `baseName` to exclude extensions (uloc_getBaseName) while
  `toString()` keeps them.
- Conformance test extended with all of the above; passes native + WASIX.

**Lowest-priority items DONE (2026-07-01):** DisplayNames `dateTimeField` (udatpg_getFieldDisplayName);
RelativeTimeFormat `formatToParts` (UFormattedRelativeDateTime + number-category field iteration, with
`unit` on numeric parts); NumberFormat `roundingMode` (all 9 ECMA modes → rounding-mode-* skeleton) and
`trailingZeroDisplay: stripIfInteger` (precision `/w`). Verified native + WASIX.

**Deliberately skipped (genuinely higher effort, rare):** PluralRules `selectRange` — needs the whole
`UFormattedNumberRange` / number-range-formatter API (`unumberrangeformatter.h`), not just a plain call.
NumberFormat `roundingIncrement`/`roundingPriority` (decimal-increment skeleton computation). These are the
only remaining ECMA-402 gaps; file a follow-up only if an app needs them.

### Phase 3 — conformance + app validation (2026-07-01)
- **Conformance probe DONE:** `test/parallel/test-edge-intl-icu.js` — runs inside the package on both the
  native CLI and WASIX lanes (standard Node test/parallel category), guarded by `common.hasIntl`. Covers all
  9 constructors + getCanonicalLocales, the 5 acceptance snippets, formatToParts, bound-method detach,
  Locale/Segmenter. Passes on `build-edge-quickjs-cli`.
- **WASIX re-verification:** in progress (`make build-quickjs-wasix`); run the probe under `wasmer`.
- **Ghost:** OUT OF SCOPE IN THIS REPO — Ghost's `start.js` Intl monkeypatch lives in the Ghost app
  artifact, not here (`javascript/` does not exist). The Intl surface Ghost needs (NumberFormat currency,
  DateTimeFormat) is covered by the conformance test; the monkeypatch removal happens in the Ghost deploy.
- **Etherpad:** the `Intl.ListFormat` blocker (ECO-381, folded into this issue) is RESOLVED and verified.
  But `js-etherpad` stays edge-skipped for the **other**, separately-tracked blockers the Makefile names
  (GC use-after-free on native + WASIX esbuild call, `Makefile:407-409`). Un-skipping now would fail on
  those, so it is intentionally left skipped until they land. Intl is no longer a gating factor.
_Original design notes:_
- New `src/edge_intl.{cc,h}`; delete `edge_intl_fallback.*`; update `src/CMakeLists.txt:7` and the call site
  at `edge_runtime.cc:2631` (`EdgeInstallIntl`). Remove the `HasUsableDateTimeFormat` short-circuit (we now
  always install the real one).
- Shared helpers: locale negotiation (`uloc_forLanguageTag` + `uloc_acceptLanguageFromHTTP`-style best-fit),
  options-bag parsing, `UErrorCode` → JS `TypeError`/`RangeError`, UTF-8↔UTF-16 (`u_strToUTF8`/`u_strFromUTF8`),
  a generic `supportedLocalesOf`, and a generic `formatToParts` field-position→array builder.
- Install a single `Intl` object carrying all constructors + `getCanonicalLocales`.

### Phase 2 — constructors (ICU C-API mapping)
Land order = app unblockers first, then completeness:
**(a) ListFormat → NumberFormat → DateTimeFormat(real)** (Etherpad + Ghost), then **(b) the rest**.

| Constructor | ICU API | Notes |
|---|---|---|
| `ListFormat` | `ulistfmt_openForType` + `ulistfmt_formatStringsToResult` | Etherpad/oidc-provider unblocker. `format`, `formatToParts`. |
| `NumberFormat` | `unumf_openForSkeletonAndLocale` / `unum_*` | `format`, `formatToParts`, `resolvedOptions`; currency/percent/unit; Ghost currency call. |
| `DateTimeFormat` | `udat_open` + `ucal_*` | Real `resolvedOptions().timeZone` (host TZ or documented UTC fallback); `formatToParts` via field iterator. Replaces the en-US stub. |
| `PluralRules` | `uplrules_openForType` + `uplrules_select` | `select`, static `supportedLocalesOf`. |
| `Collator` | `ucol_open` + `ucol_strcoll` | `compare`, sensitivity/usage options. |
| `RelativeTimeFormat` | `ureldatefmt_open` + `ureldatefmt_format`/`formatNumeric` | `format`, `formatToParts`. |
| `Locale` | `uloc_*` / `uloc_addLikelySubtags`/`minimizeSubtags` | `maximize`/`minimize`, tag properties. |
| `DisplayNames` | `uldn_open` + `uldn_*ForCode` | languages/regions/scripts/currencies. |
| `Segmenter` | `ubrk_open` (word/sentence/grapheme) | `segment` + segments iterator. |
| `getCanonicalLocales` | `uloc_canonicalize` / `uloc_forLanguageTag` | free function on `Intl`. |

### Phase 3 — conformance + app validation
- Add a smoke/conformance probe that runs **inside** the `wasmer/edgejs-quickjs` package (per acceptance
  criteria — not just host Node), covering every constructor + the issue's exact snippets. Run it on both the
  WASIX artifact and the native CLI.
- **Ghost:** remove the Intl monkeypatch from `javascript/ghost/start.js`; confirm it still boots to serve
  `/ghost/`.
- **Etherpad:** remove the edge-skip in the Makefile; confirm it boots past `new Intl.ListFormat()`.

## Acceptance criteria mapping (from the issue)
- `Intl.NumberFormat/DateTimeFormat/PluralRules/Collator` report `function` → Phase 2.
- `*.supportedLocalesOf('en')` do not throw → generic helper, Phase 1.
- `new Intl.NumberFormat('en',{style:'currency',currency:'USD'}).format(12.3)` → NumberFormat + `unumf`.
- `new Intl.DateTimeFormat('en-GB',{timeZoneName:'short'}).formatToParts(new Date())` returns parts → DTF field iterator.
- `new Intl.DateTimeFormat().resolvedOptions().timeZone` returns real TZ or documented fallback → Phase 0/DTF.
- Ghost boots with the monkeypatch removed → Phase 3.

## Risks / open questions
- **ICU data activation (Phase 0)** is the true unknown and gates everything — resolve before estimating the rest.
- **Binary size:** +~11 MB on native; confirm acceptable for `build-edge-quickjs-cli` distribution.
- **Timezone source under WASIX/sandbox:** if no host TZ is available, `resolvedOptions().timeZone` must
  return a documented stable fallback (UTC) rather than throwing.
- **`freeze_intrinsics.js`** already expects these constructors to exist — the new install aligns with it
  (previously they were simply absent).
- Confirm the WASIX ICU compile flags (`UCONFIG_ONLY_HTML_CONVERSION=1`, `USE_CHROMIUM_ICU=1`) don't strip
  i18n data needed for the full surface; native `EdgeICU.cmake` defines differ and must match.

## Files to touch
- `src/edge_intl.{cc,h}` (new) — replaces `src/edge_intl_fallback.{cc,h}` (delete).
- `src/edge_runtime.cc:2631` — call `EdgeInstallIntl`; drop `HasUsableDateTimeFormat` gate.
- `src/CMakeLists.txt` — swap the compiled source.
- `cmake/EdgeICU.cmake` — native real-data path (drop stubdata for quickjs target) + shared activation.
- `wasix/cmake/icu_wasix.cmake` — data activation wiring if Phase 0 finds it missing.
- ICU data activation site (bz2 decompress → `udata_setCommonData`), shared by both targets.
- Conformance probe under the quickjs package; `javascript/ghost/start.js` (remove shim); Makefile (un-skip Etherpad).
