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
$ragdollVersion = '1.3-combat.3'
New-Item -ItemType Directory -Force $logs | Out-Null

function Run([string] $Program, [string[]] $Arguments, [string] $LogName) {
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $logs $LogName)
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE; see $LogName." }
}

function Assert-GoreAssets([string] $Directory) {
    $manifest = Get-Content (Join-Path $PSScriptRoot 'brutal-gore-dds-sha256.json') -Raw | ConvertFrom-Json
    foreach ($asset in $manifest.PSObject.Properties) {
        $path = Join-Path $Directory ("resources/vfs/textures/brutal/gore/" + $asset.Name)
        if (-not (Test-Path $path)) { throw "Missing corpse/cut DDS: $($asset.Name)" }
        if ((Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.Value) {
            throw "Corpse/cut DDS differs from the uploaded original: $($asset.Name)"
        }
    }
}

function Invoke-WithBuildReports([scriptblock] $BuildAction, [string] $ReportStage = 'build') {
    # A separate PowerShell process can publish log tails while Ninja runs.
    # Each report finishes before another starts; request a clean stop before
    # the next workflow phase writes to the same disposable report checkout.
    $stopFile = Join-Path $env:RUNNER_TEMP ("ragdoll-report-stop-" + [Guid]::NewGuid().ToString('N'))
    $reportScript = Join-Path $PSScriptRoot 'ragdoll-report.ps1'
    $heartbeat = Start-Job -ArgumentList $reportScript, $stopFile, $ReportStage -ScriptBlock {
        param([string] $ReportScript, [string] $StopFile, [string] $ReportStage)
        $ErrorActionPreference = 'Stop'
        while (-not (Test-Path $StopFile)) {
            for ($second = 0; $second -lt 60 -and -not (Test-Path $StopFile); ++$second) {
                Start-Sleep -Seconds 1
            }
            if (Test-Path $StopFile) { break }
            try {
                & $ReportScript -Stage $ReportStage -Result running
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
        Assert-GoreAssets $build
        # Resource lists are explicit. Fail before compiling if installation
        # would omit artwork or a shader needed by the blood renderer.
        $bloodAssets = Get-Content (Join-Path $PSScriptRoot 'brutal-blood-dds-sha256.json') -Raw | ConvertFrom-Json
        foreach ($asset in $bloodAssets.PSObject.Properties) {
            $assetPath = Join-Path $build ("resources/vfs/textures/brutal/blood/" + $asset.Name)
            if (-not (Test-Path $assetPath)) { throw "Blood DDS absent after configure: $($asset.Name)" }
            if ((Get-FileHash $assetPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.Value) {
                throw "Configured blood DDS differs from the uploaded original: $($asset.Name)"
            }
        }
        foreach ($shader in @('brutalblood_shadow.vert', 'brutalblood_shadow.frag')) {
            $assetPath = Join-Path $build "resources/shaders/compatibility/$shader"
            if (-not (Test-Path $assetPath)) { throw "Blood shader absent after configure: $shader" }
        }
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
        # Compile the changed native paths before the full engine. This catches
        # MSVC-only errors in a small early pass; Ninja reuses these objects later.
        Run 'cmake' @('--build', $build, '--parallel', '3', '--target',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/combat.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/character.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/actors.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/creaturestats.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwclass/npc.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwclass/creature.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwinput/actionmanager.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwinput/bindingsmanager.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwlua/inputbindings.cpp.obj',
            'apps/openmw/CMakeFiles/openmw-lib.dir/mwrender/animation.cpp.obj',
            'apps/openmw_tests/CMakeFiles/openmw-tests.dir/mwphysics/testitems.cpp.obj') 'preflight-regressions.log'
    }
    Build {
        Invoke-WithBuildReports {
            Run 'cmake' @('--build', $build, '--parallel', '3', '--target', 'openmw') 'build.log'
        }
    }
    Test {
        Invoke-WithBuildReports -BuildAction {
            Run 'cmake' @('--build', $build, '--parallel', '3', '--target', 'components-tests', 'openmw-tests') 'build-component-tests.log'
        } -ReportStage tests
        Copy-Runtime $build
        Run "$build/components-tests.exe" @('--gtest_color=no') 'components-tests.log'
        Run "$build/openmw-tests.exe" @('--gtest_color=no', '--gtest_filter=ItemPhysics*') 'item-integration-tests.log'
    }
    Package {
        $testResult = if ($env:RAGDOLL_TEST_RESULT) { $env:RAGDOLL_TEST_RESULT } else { 'not reported' }
        $shortSha = $env:GITHUB_SHA.Substring(0, 12)
        $packageName = "Brutal-Morrowind-$ragdollVersion-OpenMW-0.51-Windows-x64-$shortSha"
        $install = Join-Path $source "package/$packageName"
        Run 'cmake' @('--install', $build, '--prefix', $install, '--config', 'Release') 'install.log'
        Copy-Runtime $install
        Copy-CyrillicLaunchers $install
        Assert-GoreAssets $install
        foreach ($required in @('openmw.exe', 'defaults.bin', 'openmw.cfg', 'resources', 'LICENSE.txt', 'SDL2.dll', 'MyGUIEngine.dll')) {
            if (-not (Test-Path (Join-Path $install $required))) { throw "Incomplete package: $required is missing." }
        }
        # Check the exact uploaded DXT5 artwork after installation, including
        # original filenames containing spaces and parentheses.
        $bloodAssets = Get-Content (Join-Path $PSScriptRoot 'brutal-blood-dds-sha256.json') -Raw | ConvertFrom-Json
        foreach ($asset in $bloodAssets.PSObject.Properties) {
            $assetPath = Join-Path $install ("resources/vfs/textures/brutal/blood/" + $asset.Name)
            if (-not (Test-Path $assetPath)) { throw "Missing blood DDS: $($asset.Name)" }
            if ((Get-FileHash $assetPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.Value) {
                throw "Blood DDS differs from the uploaded original: $($asset.Name)"
            }
        }
        foreach ($shader in @('brutalblood_shadow.vert', 'brutalblood_shadow.frag')) {
            $assetPath = Join-Path $install "resources/shaders/compatibility/$shader"
            if (-not (Test-Path $assetPath)) { throw "Missing installed blood shader: $shader" }
            $expectedPath = Join-Path $source "files/shaders/compatibility/$shader"
            if ((Get-FileHash $assetPath -Algorithm SHA256).Hash -ne (Get-FileHash $expectedPath -Algorithm SHA256).Hash) {
                throw "Installed blood shader differs from source: $shader"
            }
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
        Copy-Item "$source/docs/brutal-morrowind.md" (Join-Path $install 'BRUTAL-MORROWIND-README.md')
        Copy-Item "$source/docs/physics-tz-audit.md" (Join-Path $install 'PHYSICS-STATUS.md')
        Copy-Item "$source/docs/dismemberment.md" (Join-Path $install 'dismemberment.md')
        @"
Brutal Morrowind $ragdollVersion runtime for OpenMW 0.51.0 / Windows x64.
Source: https://github.com/$($env:GITHUB_REPOSITORY)/tree/$($env:GITHUB_SHA)
Commit: $($env:GITHUB_SHA)
Build: https://github.com/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID)
Dependencies: pinned OpenMW vcpkg bundle 2026-02-24 (SHA512 in licenses/dependency-manifest.txt).
Build configuration: MSVC 2022 x64, Release, runtime only (no launcher / editor).
Automated component/integration tests: $testResult (see VALIDATION.txt).

Extract the entire directory and launch its openmw.exe. Existing OpenMW user
configuration is reused. Morrowind game data is required and is not included.
This is the Brutal Morrowind modified engine, not a Lua mod or an official OpenMW release.
Keep the regular OpenMW installation and use a separate copy of your saves.
See RAGDOLL-README.md for feature configuration, limits and manual test steps.
"@ | Set-Content (Join-Path $install 'BUILD-INFO.txt') -Encoding utf8
        $validation = Join-Path $install 'validation'
        New-Item -ItemType Directory -Force $validation | Out-Null
        foreach ($name in @('ragdoll-tests.log', 'components-tests.log', 'item-integration-tests.log')) {
            $testLog = Join-Path $logs $name
            if (Test-Path $testLog) { Copy-Item $testLog (Join-Path $validation $name) }
        }
        @"
Automated component/integration test step: $testResult.
Run: https://github.com/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID)
The validation directory contains the test output available at packaging time.
A failed or skipped test step does not count as a passing build. Such a ZIP is
retained as a diagnostic candidate, with the failed job status preserved.
These automated tests do not constitute an in-game playtest with Morrowind data.
"@ | Set-Content (Join-Path $install 'VALIDATION.txt') -Encoding utf8
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
        [ordered]@{ file = "$packageName.zip"; sha256 = $hash; bytes = (Get-Item $zip).Length; sourceCommit = $env:GITHUB_SHA; ragdollVersion = $ragdollVersion; modName = 'Brutal Morrowind'; testResult = $testResult } |
            ConvertTo-Json | Set-Content (Join-Path $dist 'package.json') -Encoding utf8
        Run 'git' @('-c', 'core.autocrlf=false', 'archive', '--format=zip', "--output=$dist/Brutal-Morrowind-$ragdollVersion-Source-$shortSha.zip", 'HEAD') 'source-package.log'
    }
    Publish {
        $tag = "brutal-$ragdollVersion"
        $notes = Join-Path $logs 'release-notes.md'
        $releaseText = Get-Content (Join-Path $source 'docs/combat-release-notes.md') -Raw
        $releaseText += "`nSource commit: $($env:GITHUB_SHA)`nBuild: https://github.com/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID)`n"
        $releaseText | Set-Content $notes -Encoding utf8
        $assets = @(Get-ChildItem $dist -Filter '*.zip' -File | ForEach-Object FullName)
        $assets += Join-Path $dist 'SHA256SUMS'
        Run 'gh' (@('release', 'create', $tag, '--repo', $env:GITHUB_REPOSITORY, '--target', $env:GITHUB_SHA, '--prerelease', '--title', "Brutal Morrowind $ragdollVersion - Windows x64 ($($env:GITHUB_SHA.Substring(0, 12)))", '--notes-file', $notes) + $assets) 'publish.log'
        "https://github.com/$($env:GITHUB_REPOSITORY)/releases/tag/$tag" | Set-Content (Join-Path $dist 'release-url.txt') -Encoding ascii
    }
}
