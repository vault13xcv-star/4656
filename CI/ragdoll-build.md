# Experimental Windows x64 build

Push the modified engine to a branch matching `experimental/ragdoll-*` to run
`.github/workflows/realcombat-ragdoll.yml`. GitHub Actions must be enabled for the
repository. A manual `workflow_dispatch` is also supported once GitHub exposes
the workflow. No personal access token or repository secret is needed: the job
uses its short-lived `GITHUB_TOKEN` with `contents: write`.

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

Successful jobs publish an immutable GitHub prerelease containing the runtime
ZIP, its SHA256 checksum and the complete corresponding engine source archive.
Actions also stores these as a 30-day artifact. Physics, synthetic OSG skeleton and component tests,
the installed executable's `--version` command, and ZIP integrity must pass
before publication. This is not an in-game playtest.

The standalone tests are configured with `-DRAGDOLL_TEST_RENDERING=ON` and the
same vcpkg toolchain/prefix as the engine, so the Bullet and OpenSceneGraph ABIs
match the runtime. Release tags begin with `ragdoll-windows-`; they do not match
the upstream release workflow's `openmw-**` tag trigger.

## Poll without GitHub API access

Each run creates its own report branch:

```
build-results/ragdoll-<full source commit>-<run ID>-<attempt>
```

For example, after pushing source commit `SOURCE_SHA`:

```sh
git ls-remote origin 'refs/heads/build-results/ragdoll-SOURCE_SHA-*'
git fetch origin refs/heads/build-results/ragdoll-SOURCE_SHA-RUN_ID-1
git show FETCH_HEAD:status.json
git show FETCH_HEAD:logs/build.log
```

Replace the placeholders with the actual values. `status.json` identifies the
phase, overall result, source commit, run URL, package checksum and release URL.
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
