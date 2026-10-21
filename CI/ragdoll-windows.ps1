param(
    [Parameter(Mandatory)][ValidateSet('Dependencies', 'Configure', 'RagdollTests', 'Build', 'Test', 'Package', 'Publish')][string] $Phase
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = Split-Path $PSScriptRoot -Parent
Set-Location $source
$depsName = 'vcpkg-x64-windows-2022-2026-02-24'
$deps = Join-Path $source "deps/$depsName"
$prefix = Join-Path $deps 'installed/x64-windows'
$build = Join-Path $source 'build'
$tests = Join-Path $source 'build-ragdoll-tests'
$logs = Join-Path $source 'ragdoll-logs'
$dist = Join-Path $source 'dist'
$ragdollVersion = '0.6.0-preview.3'
New-Item -ItemType Directory -Force $logs | Out-Null

function Run([string] $Program, [string[]] $Arguments, [string] $LogName) {
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $logs $LogName)
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE; see $LogName." }
}

function Invoke-WithBuildReports([scriptblock] $BuildAction) {
    # A separate PowerShell process can publish log tails while Ninja runs.
    # Each report finishes before another starts; request a clean stop before
    # the next workflow phase writes to the same disposable report checkout.
    $stopFile = Join-Path $env:RUNNER_TEMP ("ragdoll-report-stop-" + [Guid]::NewGuid().ToString('N'))
    $reportScript = Join-Path $PSScriptRoot 'ragdoll-report.ps1'
    $heartbeat = Start-Job -ArgumentList $reportScript, $stopFile -ScriptBlock {
        param([string] $ReportScript, [string] $StopFile)
        $ErrorActionPreference = 'Stop'
        while (-not (Test-Path $StopFile)) {
            for ($second = 0; $second -lt 60 -and -not (Test-Path $StopFile); ++$second) {
                Start-Sleep -Seconds 1
            }
            if (Test-Path $StopFile) { break }
            try {
                & $ReportScript -Stage build -Result running
            } catch {
                # Reporting is auxiliary. The foreground build and final
                # report still determine success if GitHub has a brief outage.
                Write-Warning "Build heartbeat could not be published: $($_.Exception.Message)"
            }
        }
    }
    try {
        & $BuildAction
    } finally {
        New-Item -ItemType File -Path $stopFile -Force | Out-Null
        $finished = Wait-Job $heartbeat -Timeout 90
        if (-not $finished) { Stop-Job $heartbeat }
        Receive-Job $heartbeat -ErrorAction Continue
        Remove-Job $heartbeat -Force
        Remove-Item $stopFile -Force -ErrorAction SilentlyContinue
    }
}

function Copy-Runtime([string] $Destination) {
    New-Item -ItemType Directory -Force $Destination | Out-Null
    Copy-Item "$prefix/bin/*.dll" $Destination -Force
    Copy-Item "$prefix/bin/Release/MyGUIEngine.dll" $Destination -Force
    $plugins = Join-Path $Destination 'osgPlugins-3.6.5'
    New-Item -ItemType Directory -Force $plugins | Out-Null
    Copy-Item "$prefix/bin/osgPlugins-3.6.5/*.dll" $plugins -Force
    # App-local CRT allows this portable runtime to launch without an installer.
    $crt = Get-ChildItem "$env:VCToolsRedistDir/x64" -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
    if (-not $crt) { throw 'The x64 redistributable MSVC runtime was not found.' }
    Copy-Item (Join-Path $crt.FullName '*.dll') $Destination -Force
}

function Copy-CyrillicLaunchers([string] $Destination) {
    foreach ($fontFile in @(
        'MysticCards.ttf', 'MysticCards.omwfont', 'MysticCardsFontLicense.txt',
        'DejaVuLGCSansMono.ttf', 'DejaVuLGCSansMono.omwfont', 'DejaVuFontLicense.txt'
    )) {
        $installedFont = Join-Path $Destination "resources/vfs/fonts/$fontFile"
        if (-not (Test-Path $installedFont -PathType Leaf) -or (Get-Item $installedFont).Length -eq 0) {
            throw "Incomplete Cyrillic font package: $fontFile is missing or empty."
        }
    }
    foreach ($launcherFile in @('OpenMW-Russian.cmd', 'OpenMW-Cyrillic-Fonts.cmd')) {
        Copy-Item (Join-Path $source "files/windows/$launcherFile") (Join-Path $Destination $launcherFile) -Force
    }
    Copy-Item (Join-Path $source 'docs/realcombat-cyrillic.md') (Join-Path $Destination 'CYRILLIC-README.md') -Force
}

switch ($Phase) {
    Dependencies {
        $manifest = Get-Content (Join-Path $PSScriptRoot 'ragdoll-deps-manifest.txt')
        $url = $manifest[0].Trim()
        $fields = $manifest[1].Trim() -split '\s+'
        $expectedHash = $fields[0]
        $fileName = $fields[1]
        if ($expectedHash -notmatch '^[0-9a-fA-F]{128}$' -or $fileName -ne "$depsName.7z") {
            throw 'Invalid pinned dependency manifest.'
        }
        if ($url -ne "https://github.com/OpenMW/openmw-deps-build/releases/download/2026-02-24/$fileName") {
            throw 'Unexpected dependency archive URL.'
        }
        New-Item -ItemType Directory -Force (Join-Path $source 'deps') | Out-Null
        $archive = Join-Path $source "deps/$fileName"
        Run 'curl.exe' @('--fail', '--retry', '3', '--location', '--output', $archive, $url) 'dependencies-download.log'
        $actualHash = (Get-FileHash $archive -Algorithm SHA512).Hash
        if ($actualHash -ne $expectedHash) { throw 'Dependency archive SHA512 mismatch.' }
        Run '7z' @('x', '-y', "-o$deps", $archive) 'dependencies-extract.log'
        if (-not (Test-Path "$deps/scripts/buildsystems/vcpkg.cmake")) { throw 'Dependency extraction is incomplete.' }
    }
    Configure {
        $common = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_TOOLCHAIN_FILE=$deps/scripts/buildsystems/vcpkg.cmake")
        $options = @(
            '-S', $source, '-B', $build,
            '-DBUILD_OPENMW=ON', '-DBUILD_COMPONENTS_TESTS=ON',
            '-DBUILD_LAUNCHER=OFF', '-DBUILD_WIZARD=OFF', '-DBUILD_OPENCS=OFF',
            '-DBUILD_MWINIIMPORTER=OFF', '-DBUILD_ESSIMPORTER=OFF', '-DBUILD_BSATOOL=OFF',
            '-DBUILD_ESMTOOL=OFF', '-DBUILD_NIFTEST=OFF', '-DBUILD_DOCS=OFF',
            '-DBUILD_NAVMESHTOOL=OFF', '-DBUILD_BULLETOBJECTTOOL=OFF',
            '-DBUILD_BENCHMARKS=OFF', '-DBUILD_OPENCS_TESTS=OFF', '-DBUILD_OPENMW_TESTS=ON',
            '-DDEPLOY_QT_TRANSLATIONS=OFF', '-DOPENMW_LTO_BUILD=OFF',
            '-DOPENMW_USE_SYSTEM_SQLITE3=OFF', '-DOPENMW_USE_SYSTEM_YAML_CPP=OFF',
            "-DLuaJit_INCLUDE_DIR=$prefix/include/luajit", "-DLuaJit_LIBRARY=$prefix/lib/lua51.lib"
        )
        Run 'cmake' ($options + $common) 'configure.log'
        Run 'cmake' (@('-S', "$source/components/ragdoll", '-B', $tests,
            '-DRAGDOLL_TEST_RENDERING=ON', "-DCMAKE_PREFIX_PATH=$prefix") + $common) 'configure-ragdoll-tests.log'
    }
    RagdollTests {
        # Exercise the exact Windows Bullet build before spending time compiling
        # the whole engine. Keep this mandatory and preserve every test failure.
        Run 'cmake' @('--build', $tests, '--parallel', '3') 'build-ragdoll-tests.log'
        Copy-Runtime $tests
        Run 'ctest' @('--test-dir', $tests, '--output-on-failure') 'ragdoll-tests.log'
        Run "$tests/ragdoll-render-tests.exe" @('--benchmark') 'synthetic-load.log'
    }
    Build {
        Invoke-WithBuildReports {
            Run 'cmake' @('--build', $build, '--parallel', '3', '--target', 'openmw', 'components-tests', 'openmw-tests') 'build.log'
        }
    }
    Test {
        Copy-Runtime $build
        Run "$build/components-tests.exe" @('--gtest_color=no') 'components-tests.log'
        Run "$build/openmw-tests.exe" @('--gtest_color=no', '--gtest_filter=ItemPhysics*') 'item-integration-tests.log'
    }
    Package {
        $shortSha = $env:GITHUB_SHA.Substring(0, 12)
        $packageName = "OpenMW-0.51-RealCombat-Ragdoll-$ragdollVersion-Windows-x64-$shortSha"
        $install = Join-Path $source "package/$packageName"
        Run 'cmake' @('--install', $build, '--prefix', $install, '--config', 'Release') 'install.log'
        Copy-Runtime $install
        Copy-CyrillicLaunchers $install
        foreach ($required in @('openmw.exe', 'defaults.bin', 'openmw.cfg', 'resources', 'LICENSE.txt', 'SDL2.dll', 'MyGUIEngine.dll')) {
            if (-not (Test-Path (Join-Path $install $required))) { throw "Incomplete package: $required is missing." }
        }
        Get-ChildItem $install -Filter '*.pdb' -Recurse | Remove-Item -Force
        $licences = Join-Path $install 'licenses/dependencies'
        New-Item -ItemType Directory -Force $licences | Out-Null
        foreach ($license in Get-ChildItem "$prefix/share/*/copyright" -File) {
            $library = $license.Directory.Name
            Copy-Item $license.FullName (Join-Path $licences "$library.txt")
        }
        Copy-Item (Join-Path $PSScriptRoot 'ragdoll-deps-manifest.txt') (Join-Path $install 'licenses/dependency-manifest.txt')
        if (Test-Path "$source/docs/realcombat-ragdoll.md") {
            Copy-Item "$source/docs/realcombat-ragdoll.md" (Join-Path $install 'RAGDOLL-README.md')
        }
        Copy-Item "$source/docs/physics-tz-audit.md" (Join-Path $install 'PHYSICS-STATUS.md')
        Copy-Item "$source/docs/dismemberment.md" (Join-Path $install 'dismemberment.md')
        @"
Experimental humanoid ragdoll $ragdollVersion runtime for OpenMW 0.51.0 / Windows x64.
Source: https://github.com/$($env:GITHUB_REPOSITORY)/tree/$($env:GITHUB_SHA)
Commit: $($env:GITHUB_SHA)
Build: https://github.com/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID)
Dependencies: pinned OpenMW vcpkg bundle 2026-02-24 (SHA512 in licenses/dependency-manifest.txt).
Build configuration: MSVC 2022 x64, Release, runtime only (no launcher / editor).

Extract the entire directory and launch its openmw.exe. Existing OpenMW user
configuration is reused. Morrowind game data is required and is not included.
This is an experimental engine build, not a Lua mod or an official OpenMW release.
Keep the regular OpenMW installation and use a separate copy of your saves.
See RAGDOLL-README.md for feature configuration, limits and manual test steps.
"@ | Set-Content (Join-Path $install 'BUILD-INFO.txt') -Encoding utf8
        # Launch from the installed tree so missing app-local DLLs fail packaging.
        $versionOut = Join-Path $logs 'installed-version.log'
        $versionErr = Join-Path $logs 'installed-version-errors.log'
        $process = Start-Process -FilePath (Join-Path $install 'openmw.exe') -ArgumentList '--version' -WorkingDirectory $install -Wait -PassThru -RedirectStandardOutput $versionOut -RedirectStandardError $versionErr
        if ($process.ExitCode -ne 0) { throw "Installed runtime failed --version (exit $($process.ExitCode))." }
        Run (Join-Path $install 'OpenMW-Russian.cmd') @('--version') 'installed-russian-version.log'
        Run (Join-Path $install 'OpenMW-Cyrillic-Fonts.cmd') @('--version') 'installed-cyrillic-fonts-version.log'
        New-Item -ItemType Directory -Force $dist | Out-Null
        $zip = Join-Path $dist "$packageName.zip"
        Push-Location (Split-Path $install -Parent)
        try { Run '7z' @('a', '-tzip', '-mx=7', $zip, $packageName) 'package.log' } finally { Pop-Location }
        Run '7z' @('t', $zip) 'package-integrity.log'
        $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $packageName.zip" | Set-Content (Join-Path $dist 'SHA256SUMS') -Encoding ascii
        [ordered]@{ file = "$packageName.zip"; sha256 = $hash; bytes = (Get-Item $zip).Length; sourceCommit = $env:GITHUB_SHA; ragdollVersion = $ragdollVersion } |
            ConvertTo-Json | Set-Content (Join-Path $dist 'package.json') -Encoding utf8
        Run 'git' @('archive', '--format=zip', "--output=$dist/OpenMW-Ragdoll-$ragdollVersion-Source-$shortSha.zip", 'HEAD') 'source-package.log'
    }
    Publish {
        $tag = "ragdoll-windows-$ragdollVersion-$($env:GITHUB_SHA.Substring(0, 12))-$($env:GITHUB_RUN_ID)-$($env:GITHUB_RUN_ATTEMPT)"
        $notes = Join-Path $logs 'release-notes.md'
        @"
Experimental OpenMW 0.51.0 runtime for Windows x64 with humanoid ragdolls $ragdollVersion.

Download the **Windows-x64 ZIP**, extract it fully, and read RAGDOLL-README.md.
This build reuses your OpenMW user configuration; copyrighted game data is not included.
The launcher/editor are not bundled. Keep your stable installation and separate test saves.

Source commit: $($env:GITHUB_SHA)
Compiler: MSVC 2022 x64; dependency archive verified against the checked-in SHA512 manifest.
Version 0.5 adds independent active budgets and timers, saved physical poses, remaining-time restoration, weight/Strength impulses, creature profiles, buoyancy and longer elemental effects. Read PHYSICS-STATUS.md for unfinished specification items.
Automated checks: compiled runtime, component tests, ragdoll and item physics, synthetic skeleton and lethal-hit attribution tests, installed --version smoke checks, ZIP integrity.
These checks do not replace testing animations and gameplay inside Morrowind.

Corresponding engine sources are attached. Dependency notices are in the runtime's licenses directory.
"@ | Set-Content $notes -Encoding utf8
        $assets = @(Get-ChildItem $dist -Filter '*.zip' -File | ForEach-Object FullName)
        $assets += Join-Path $dist 'SHA256SUMS'
        Run 'gh' (@('release', 'create', $tag, '--repo', $env:GITHUB_REPOSITORY, '--target', $env:GITHUB_SHA, '--prerelease', '--title', "OpenMW 0.51 humanoid ragdoll $ragdollVersion - Windows x64 ($($env:GITHUB_SHA.Substring(0, 12)))", '--notes-file', $notes) + $assets) 'publish.log'
        "https://github.com/$($env:GITHUB_REPOSITORY)/releases/tag/$tag" | Set-Content (Join-Path $dist 'release-url.txt') -Encoding ascii
    }
}
