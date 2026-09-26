# Framework CI Gatsby baseline build failures

| | | Remarks |
| --- | --- | --- |
| **Status** | 🟢 | Both Gatsby builds and focused Node/QuickJS HTTP checks pass with the install fix. |
| **Severity** | Medium | Blocks framework CI despite passing QuickJS runtime cases. |

## Evidence

Edge PR #155 at `dd8454d0`, run `36205789723`, job `108301917838`, fails
`Test framework apps (QuickJS native)`. The step runs both the Node.js baseline
and QuickJS. Node passes 12 examples but fails `js-gatsby-staticsite` and
`js-gatsby-staticsite2`; QuickJS passes 11 and has no failed cases. The two
Gatsby examples are skipped on QuickJS because their baseline builds failed.
The harness correctly returns failure if any stage fails.

Both Node builds report `error Generating JavaScript bundles failed`. Their
full `.framework-test/logs/*.node.build.log` files are not uploaded by CI.
The runner uses Node 24.21.0 and pnpm@latest, with examples pinned to
`b9f94d8ca5896ccf812a071a8e86a6c0e542c64f`.

## Action plan

1. Reproduce the exact two examples and harness under Linux with Node 24.21.0,
   first using native arm64 on this host. Inspect the full bundler diagnostics.
2. Correct the underlying build/fixture configuration with minimal changes.
   Preserve failure reporting and framework coverage.
3. Verify both Node builds and their runtime routes, then QuickJS serving of
   the same artifacts. Preserve actionable build diagnostics for future CI.

Initial job log: `/tmp/edge155-quickjs-framework-108301917838.log`.
Linux reproduction container: `edge-gatsby-ci-repro`, Ubuntu on arm64.
The repository is mounted read-only at `/source`; copied fixtures live in
`/work` so the reproduction does not rewrite the examples checkout.

## Diagnosis and fix

Both failures reproduce with Node 24.21.0, pnpm 12.6.0, and Gatsby 5.16.1.
The full webpack output reports missing `mitt`, `shallow-compare`,
`@gatsbyjs/reach-router`, `gatsby-react-router-scroll`, `gatsby-link`,
`gatsby-script`, and `prop-types` from generated files under the app's `.cache`.
These packages are installed under Gatsby's dependency tree, but pnpm's
isolated layout does not expose them to generated code outside `node_modules`.

The shared framework installer now requests
`--config.shamefully-hoist=true` only when the project declares Gatsby. This
uses pnpm's supported install configuration without editing app sources,
skipping examples, or changing Edge/QuickJS module resolution. A generated-code
fixture verifies that Gatsby can resolve its transitive dependencies and that
other frameworks retain their existing isolation. The Gatsby case fails before
the change and passes afterward.

Failure summaries now include the failed stage name and prioritize webpack's
`Can't resolve` diagnostic over the generic bundling failure.

## Verification

- The four package-manager integration tests pass with pnpm 10.34.5, 11.28.0,
  and 12.6.0.
- Both actual Gatsby examples build on Linux and pass all five Node.js route
  checks, including the second site's DSG route.
- The focused Node-to-QuickJS WASIX framework matrix passes both examples,
  including all four QuickJS static routes. The DSG route remains Node-only
  per the existing route fixture.
- The examples submodule and framework skip lists remain unchanged.

The extra local WASIX HTTP check used Wasmer 7.4.2 and the existing QuickJS
artifact with `OPENSSL_CONF=/dev/null`: the installed Linux CLI failed to read
the package's OpenSSL configuration on startup without this override. No TLS
behavior is covered by that HTTP check, and no such override was added to CI.
The failing job under investigation is native QuickJS; its Gatsby build failure
reproduces and is fixed independently of this extra WASIX check.

Evidence is saved under `target/edge-framework-gatsby/` in the SDK checkout.
