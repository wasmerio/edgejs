# Releasing EdgeJS

EdgeJS uses one version for the native V8 and QuickJS executables and the
`wasmer/edgejs` and `wasmer/edgejs-quickjs` WASIX packages. The release baseline
is `0.2.5`, which was published to both registries before release automation
was added. The source package manifests retain their earlier development
versions until the first generated release PR updates them.

## Maintainer setup

Make these existing secret names available to the release workflow:

| Secret | Purpose |
| --- | --- |
| `RELEASE_PLEASE_GH_TOKEN` | GitHub PAT with repository contents, issues, and pull request write access. It allows release PRs to trigger ordinary CI. |
| `DEV_BACKEND_CIUSER_TOKEN` | Publish `wasmer/edgejs` and `wasmer/edgejs-quickjs` on wasmer.wtf. |
| `PROD_BACKEND_CIUSER_TOKEN` | Publish the same packages on wasmer.io. |

Existing Wasmer provisioning variables and secrets used by the two build/test
workflows also apply to release builds. No additional registry secret names
are introduced. Allow GitHub Actions to create pull requests in the repository
settings.

## Publish a version

1. Merge changes to `main` with Conventional Commit titles, such as
   `fix: handle pnpm config commands`. Release Please opens or updates a release
   PR with the changelog, `VERSION.txt`, release manifest, C++ version header,
   and both source package versions. Before 1.0, fixes and features advance the
   patch version; breaking changes advance the minor version.
2. Review the proposed version and changelog, wait for CI, and merge the release
   PR. This merge authorizes publishing that version.
3. The [release workflow](.github/workflows/release.yml) creates the `vX.Y.Z`
   tag and a draft GitHub release. It checks out the exact tagged commit and
   runs both existing build/test workflows, including their six native/WASIX
   jobs. Release builds report `X.Y.Z` without a Git commit suffix.
4. After all builds and tests pass, the workflow validates and uploads the six
   ZIPs and `SHA256SUMS`, validates the WASIX package directories, and publishes
   both packages to wasmer.wtf and wasmer.io. It then publishes the completed
   GitHub release. A failure leaves the release as a draft.

The GitHub assets are:

- `edge-linux-amd64.zip`
- `edge-darwin-arm64.zip`
- `edge-wasix.zip`
- `edge-quickjs-linux-amd64.zip`
- `edge-quickjs-darwin-arm64.zip`
- `edge-quickjs-wasix.zip`
- `SHA256SUMS`

WASIX ZIPs contain the same canonical package metadata and files used for
registry publication. Source development manifest names are normalized in the
release outputs to `wasmer/edgejs` and `wasmer/edgejs-quickjs`. Both packages
contain the `edge` module and use `bin/edge` as the executable.

## Recover a failed release

Select **Run workflow** on **release**, use `main`, and enter the existing draft
tag, for example `v0.2.6`. Recovery verifies the tag, source versions, and draft
release, then rebuilds and tests that exact commit. It refuses to change an
already completed release. Automatic and recovery runs are serialized.

If one registry/package was already published, recovery downloads it and
compares all unpacked files and the WEBC manifest with the newly built package.
An identical package is skipped; a different package at the same version fails
instead of overwriting it. A differing rebuild must be investigated, or a new
version released. Missing registry credentials fail publication; staging never
falls back to the production token.

Existing nightly workflows continue independently and are disabled inside
release workflow calls. Release publishing does not rely on tag events from
`GITHUB_TOKEN`: it follows Release Please's outputs directly.

## Validate changes locally

Use Python 3.11 or newer:

```sh
python3 -m unittest discover -s scripts/ci -p test_release.py
actionlint .github/workflows/*.yml
npm install --prefix /private/tmp/edge-release-validation --no-audit --no-fund \
  release-please@17.3.0 ajv@8.17.1
RELEASE_PLEASE_MODULES=/private/tmp/edge-release-validation/node_modules \
  node scripts/ci/verify-release-please.cjs
```

The release workflow runs these script/config checks on pull requests. The
Node verifier uses the actual Release Please updaters to check all three C++
version components and both TOML package versions, including preservation of
comments and dependency versions.

See [Release Please configuration](https://github.com/googleapis/release-please/blob/main/docs/customizing.md)
and [the GitHub Action](https://github.com/googleapis/release-please-action)
for commit conventions, release PRs, and extra-file updates.
