# G.O.R.E: Morrowind — Windows build and release

The current release is **G.O.R.E: Morrowind 1.0**, tag `gore-v1.0`. It is based on OpenMW 0.51. Historical Brutal Morrowind releases and tags remain available.

## Build pipeline

`.github/workflows/realcombat-ragdoll.yml` runs on `release/gore-1.0` and retained development branch patterns; it also supports workflow_dispatch. The Windows 2022 runner enables MSVC 2022 x64 and executes the phases of `CI/ragdoll-windows.ps1`.

The dependency archive is pinned in `CI/ragdoll-deps-manifest.txt` and verified with SHA512. Actions are pinned by commit. No personal access token is needed: release publication uses the runner's short-lived GITHUB_TOKEN with contents: write.

1. Configure the OpenMW runtime and standalone tests against the same Bullet/OpenSceneGraph ABI.
2. Build/run physics, render, damage-attribution and combat groups. Compile changed engine/native-test objects early; Ninja reuses them in the full build.
3. Build the `openmw` target. Save the compiled executable and resources as a checkpoint before compiling test programs.
4. Build/run component and native integration tests.
5. Install the portable runtime, include resources/DLLs/OSG plugins/app-local MSVC CRT, license notices and launch scripts. Check the uploaded DDS hashes, installed `--version`, all launch scripts and ZIP integrity. Create the matching source archive and SHA256SUMS for both ZIPs.
6. When all tests pass, publish a normal **Latest** release `gore-v1.0` with runtime, source, SHA256SUMS and package.json.

The archive is named `GORE-Morrowind-1.0-Windows-x64.zip`; sources are `GORE-Morrowind-1.0-Source.zip`. BUILD-INFO.txt records the full source commit and Actions URL. OpenMW's own version remains 0.51.0; G.O.R.E's mod version is 1.0.

Only runtime and tests are built. Game assets, launcher, editor and Qt are not included. The installed directory includes GORE-Morrowind.cmd, the Cyrillic launchers and font licenses, README.md, GORE-MORROWIND-README.md and validation logs.

Packaging preserves a compiled diagnostic candidate if a late test fails; VALIDATION.txt and package metadata retain that failure. A retained candidate is not automatically published as a passing release. Reuse it to diagnose minor late faults instead of restarting a full engine compilation.

## Main repository page

After release assets are downloaded and verified, the tested release commit is promoted with an ordinary fast-forward push to the repository's existing default branch, `downloads/real-combat-0.1.0`. The branch name is retained to preserve links. Its README is the default G.O.R.E page at the repository root. No history or old tag is overwritten. This promotion does not match the Windows build branch filter and therefore does not start another full compilation.

A `[publish-only]` documentation commit on the release branch updates release notes through the notes-only job without rebuilding the engine. Published source archives always identify the exact compiled commit.

## Reports and local builds

Each Windows run writes `status.json` and bounded log tails to:

```text
build-results/ragdoll-<full source commit>-<run ID>-<attempt>
```

The report includes source revision, stage/result, package SHA256/size and the final release URL. A successful installed executable/ZIP check and automated tests do not constitute an in-game Morrowind-data playtest.

For local engine builds use the [OpenMW build documentation](https://openmw.readthedocs.io/en/latest/manuals/installation/compile.html), CMake/Ninja and C++20 with the dependencies selected by this fork. The small physics/render/combat suite can be built from `components/ragdoll` with `-DRAGDOLL_TEST_RENDERING=ON`. On Linux, `-DRAGDOLL_TEST_GL=ON` additionally enables surfaceless graphics regressions. Do not reuse mismatched Bullet precision or incompatible third-party libraries.
