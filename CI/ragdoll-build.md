# Windows x64 build and stable release

Push the modified engine to a branch matching `experimental/ragdoll-*` to run
`.github/workflows/realcombat-ragdoll.yml`. GitHub Actions must be enabled for the
repository. The current stable source is tag `v1.1` on the default branch.
A manual `workflow_dispatch` is also supported once GitHub exposes the workflow.
No personal access token or repository secret is needed: the job uses its
short-lived `GITHUB_TOKEN` with `contents: write`.

The Windows Server 2022 runner activates MSVC 2022 x64. It downloads the same
OpenMW vcpkg bundle selected by `CI/deps_versions.msvc.sh`, but from the manifest
committed as `CI/ragdoll-deps-manifest.txt` rather than a mutable remote manifest.
The archive is verified with SHA512 before extraction. That manifest was copied
from `https://gitlab.com/OpenMW/openmw-deps/-/raw/main/windows/vcpkg-x64-windows-2022-2026-02-24-manifest.txt`.
Actions are pinned by commit. Runner/compiler updates mean this is a repeatable
build recipe, not a guarantee of byte-identical executables.

Only the runtime and tests are built, with launcher, editor and other tools
disabled, so no Qt download is needed. The package includes the installed
resources, default settings, configuration, DLLs, OSG plugins, app-local MSVC CRT,
OpenMW license, dependency copyright notices and commit/build metadata. It does
not include Morrowind assets. The whole ZIP must be extracted. The normal
OpenMW user configuration is reused; no stable installation is overwritten.
The package also includes `OpenMW-Russian.cmd` and `OpenMW-Cyrillic-Fonts.cmd`,
which select bundled Cyrillic-capable fonts, and `CYRILLIC-README.md`.
Packaging requires their existing
MysticCards/DejaVu font files and licenses and runs both launchers with
`--version` to check command-line forwarding. This does not validate rendered
text or the encoding of the user's game data.

Successful jobs publish an immutable GitHub prerelease containing the runtime
ZIP, its SHA256 checksum and the complete corresponding engine source archive.
Runtime/source ZIP names, the prerelease title, tag and package metadata include
the Brutal Morrowind version (currently `1.1`) separately from OpenMW `0.51.0`.
Actions also stores these as a 30-day artifact. Physics, synthetic OSG skeleton,
loose-item dynamics, lethal-hit attribution and component tests,
the installed executable's `--version` command, and ZIP integrity must pass
before publication. This is not an in-game playtest.

The standalone tests are configured with `-DRAGDOLL_TEST_RENDERING=ON` and the
same vcpkg toolchain/prefix as the engine, so the Bullet and OpenSceneGraph ABIs
match the runtime. All standalone test suites build and run before the
full engine compilation, so a Windows solver regression fails within minutes.
The complete engine and component tests still build and pass before packaging.
Linux validation can additionally enable `-DRAGDOLL_TEST_GL=ON` for surfaceless
EGL tests of visible fire/smoke, opaque-wall occlusion, inherited NPC shader/alpha
state and the native per-view projection in both normal and reversed depth modes.
The retained cloud helper accepts this flag:
`/workspace/openmw-toolchain/check.sh -DRAGDOLL_TEST_GL=ON`.
MSVC precompiled headers remain enabled; no unverified CMake build directory is
restored between jobs. Compiler-cache tools that reject MSVC precompiled headers
would require a separate measured build configuration before being enabled.
Stable releases use `v1.1` and `.github/workflows/brutal-release.yml`. The
manifest `CI/brutal-release.json` pins the current compiled runtime and matching
sources. Packaging verifies every executable/library/game-resource byte, updates
release documentation and publishes a normal Latest release without recompiling.
The known obsolete Windows test expectation remains disclosed in the archive.
These tags do not match the upstream workflow's `openmw-**` trigger.

## Poll without GitHub API access

Each run creates its own report branch:

```
build-results/ragdoll-<full source commit>-<run ID>-<attempt>
```

In the cloud checkout, `origin` refers to upstream OpenMW and `publish` refers
to `vault13xcv-star/4656`. After pushing source commit `SOURCE_SHA`, use the
publication remote and a dedicated report ref:

```sh
git ls-remote publish 'refs/heads/build-results/ragdoll-SOURCE_SHA-*'
git fetch --depth 1 --no-tags publish \
  +refs/heads/build-results/ragdoll-SOURCE_SHA-RUN_ID-1:refs/remotes/publish/ragdoll-monitor-RUN_ID
git show refs/remotes/publish/ragdoll-monitor-RUN_ID:status.json
git show refs/remotes/publish/ragdoll-monitor-RUN_ID:logs/build.log
```

Replace the placeholders with the actual values. `status.json` identifies the
phase, overall result, source commit, run URL, package checksum and release URL.
During compilation a background reporter requests an update every 60 seconds,
including the last compiler output, so long builds have a visible heartbeat.
A busy runner and Git publication can delay visible updates by several minutes.
It finishes before the next workflow phase can publish a report. A temporary
heartbeat failure does not interrupt compilation; phase/final reports remain
required.

The `logs` directory has only the last 180 lines per diagnostic log; full logs
remain in the workflow artifact. If the runtime ZIP is under 95 MiB, the final
report commit also contains it, and `package.gitDownload` gives a direct GitHub
download URL. This permits checksum verification through ordinary `git fetch`
when unauthenticated API/HTTP access is unavailable.

Report branches are unique to each run/attempt, and are updated by ordinary
fast-forward pushes. No source branches or existing release tags are rewritten.
The report branch pattern does not trigger the build workflow recursively.
Credentials remain in the disposable report checkout's local `.git/config` and
are never copied into a report, package or artifact.

If no report branch appears, inspect the repository's Actions page: workflow
execution may be disabled or unavailable. A failure to start a runner cannot be
reported by that runner. A job cancelled before its final step may retain the
last `running` status; the linked Actions run is authoritative.
