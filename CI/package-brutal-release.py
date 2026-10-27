#!/usr/bin/env python3
"""Promote the pinned Windows runtime without rebuilding any binaries."""
import argparse
import hashlib
import json
import subprocess
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = json.loads((ROOT / 'CI/brutal-release.json').read_text(encoding='utf-8'))


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def sha(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def package(input_zip, output):
    m = MANIFEST
    output.mkdir(parents=True, exist_ok=True)
    commit = git('rev-parse', 'HEAD').decode().strip()
    # Source archives must correspond to the executable. Only release material
    # may differ from its original source revision.
    runtime_paths = ['apps', 'components', 'files', 'extern', 'cmake', 'CMakeLists.txt']
    changed = git('diff', '--name-only', m['baselineSource'], commit, '--', *runtime_paths)
    if changed.strip():
        raise RuntimeError('Runtime source differs from the tested baseline: ' + changed.decode())
    status = json.loads(git('show', m['baselineReport'] + ':status.json').decode('utf-8-sig'))
    assert status['stage'] == 'completed' and status['result'] == 'success'
    assert status['sourceCommit'] == m['baselineSource']
    assert status['package']['sha256'] == m['baselineSha256']
    assert status['package']['testResult'] == m['baselineTestResult']
    assert status['package']['sourceCommit'] == m['binarySource']
    assert input_zip.stat().st_size == m['baselineBytes']
    assert sha(input_zip.read_bytes()) == m['baselineSha256']
    timestamp = int(git('show', '-s', '--format=%ct', commit))
    date = datetime.fromtimestamp(timestamp, timezone.utc).timetuple()[:6]
    prefix = m['runtimeFile'][:-4] + '/'
    origin = 'https://github.com/' + m['repository']
    provenance = {
        'product': 'Brutal Morrowind', 'version': m['version'], 'tag': m['tag'],
        'releaseCommit': commit, 'releaseSource': origin + '/tree/' + m['tag'],
        'binarySourceCommit': m['binarySource'],
        'correspondingBaselineSourceCommit': m['baselineSource'],
        'binaryBuild': origin + '/actions/runs/' + m['baselineRun'],
        'baselineArchiveSha256': m['baselineSha256'],
        'openmwExecutableSha256': m['executableSha256'],
        'recompiled': False, 'runtimePayloadUnchanged': True,
        'validation': m['validation'],
        'originalWindowsTestResult': 'failure',
        'knownIssues': 'KNOWN-ISSUES.md',
    }
    document_paths = {
        'README.md': 'README.md', 'README.txt': 'README.md', 'LICENSE': 'LICENSE',
        'BRUTAL-MORROWIND-README.md': 'docs/brutal-morrowind.md',
        'brutal-morrowind.md': 'docs/brutal-morrowind.md',
        'RAGDOLL-README.md': 'docs/brutal-morrowind.md',
        'dismemberment.md': 'docs/dismemberment.md',
        'docs/README.en.md': 'docs/README.en.md',
        f"docs/releases/{m['tag']}.md": f"docs/releases/{m['tag']}.md",
        'KNOWN-ISSUES.md': f"docs/releases/{m['tag']}-known-issues.md",
        f"docs/releases/{m['tag']}-known-issues.md": f"docs/releases/{m['tag']}-known-issues.md",
        'docs/brutal-morrowind.md': 'docs/brutal-morrowind.md',
        'docs/dismemberment.md': 'docs/dismemberment.md',
        'docs/upstream/README.OpenMW.md': 'docs/upstream/README.OpenMW.md',
        'CI/ragdoll-build.md': 'CI/ragdoll-build.md',
    }
    additions = {name: git('show', commit + ':' + path) for name, path in document_paths.items()}
    additions['RELEASE-INFO.json'] = json_bytes(provenance)
    additions['BUILD-INFO.txt'] = (
        f"Brutal Morrowind {m['version']} / OpenMW 0.51 / Windows x64\n"
        f"Release: {origin}/releases/tag/{m['tag']}\nRelease source: {origin}/tree/{commit}\n"
        f"Binary source commit: {m['binarySource']}\nBinary build: {provenance['binaryBuild']}\n"
        f"openmw.exe SHA256: {m['executableSha256']}\n\n"
        'The current compiled executable, libraries and gameplay resources are unchanged.\n'
        'This release changes package branding and documentation without recompilation.\n'
        'Full corresponding engine sources are provided alongside the runtime.\n'
        'Extract the entire ZIP and launch openmw.exe. An existing OpenMW configuration\n'
        'and your own Morrowind game data are required. The launcher/editor are not bundled.\n'
        'See README.md, KNOWN-ISSUES.md, RELEASE-INFO.json and the included dependency licenses.\n'
    ).encode('utf-8')
    additions['VALIDATION.txt'] = (
        f"Brutal Morrowind {m['version']} validation\n\n"
        'Original Windows test step: failure; 38/39 native integration tests passed.\n'
        'One obsolete test expected the intentionally removed hammer waist cut.\n'
        'Its source expectation is corrected; that Windows test was not rebuilt or rerun.\n'
        'All 1402 component tests and four physics/render regression groups passed.\n'
        'The validation directory retains the original test logs without alteration.\n'
        'Executable/DLL/game-resource hashes, installed startup and ZIP integrity are checked.\n'
        'No engine recompilation was performed for this stable release.\n'
        'See KNOWN-ISSUES.md and RELEASE-INFO.json for the disclosed exception.\n'
        'These checks do not constitute an in-game playtest with Morrowind data.\n'
    ).encode('utf-8')
    runtime = output / m['runtimeFile']
    original_hashes = {}
    with zipfile.ZipFile(input_zip) as before, zipfile.ZipFile(
            runtime, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=7) as after:
        assert before.testzip() is None
        executables = [name for name in before.namelist() if name.endswith('/openmw.exe')]
        assert len(executables) == 1
        old_prefix = executables[0][:-len('openmw.exe')]
        assert sha(before.read(executables[0])) == m['executableSha256']
        prior = json.loads(before.read(old_prefix + 'RELEASE-INFO.json'))
        assert prior['binarySourceCommit'] == m['binarySource']
        assert prior['correspondingSourceCommit'] == m['baselineSource']
        assert prior['nativeIntegrationTestsPassed'] == 38 and prior['nativeIntegrationTestsTotal'] == 39
        assert prior['originalWindowsTestResult'] == 'failure' and not prior['correctedWindowsTestRerun']
        for item in before.infolist():
            assert item.filename.startswith(old_prefix)
            relative = item.filename[len(old_prefix):]
            assert '..' not in Path(relative).parts
            data = before.read(item)
            original_hashes[relative] = sha(data)
            if relative in additions:
                continue
            info = zipfile.ZipInfo(prefix + relative, date)
            info.external_attr = item.external_attr
            info.compress_type = zipfile.ZIP_DEFLATED
            after.writestr(info, data)
        for name, data in additions.items():
            info = zipfile.ZipInfo(prefix + name, date)
            info.compress_type = zipfile.ZIP_DEFLATED
            after.writestr(info, data)
    with zipfile.ZipFile(runtime) as after:
        assert after.testzip() is None
        for relative, digest in original_hashes.items():
            if relative not in additions:
                assert sha(after.read(prefix + relative)) == digest, relative
        assert sha(after.read(prefix + 'openmw.exe')) == m['executableSha256']
        assert json.loads(after.read(prefix + 'RELEASE-INFO.json')) == provenance
    source = output / m['sourceFile']
    subprocess.run(['git', '-C', str(ROOT), '-c', 'core.autocrlf=false', 'archive', '--format=zip',
                    f"--prefix={m['sourceFile'][:-4]}/", '--output=' + str(source), commit], check=True)
    with zipfile.ZipFile(source) as archive:
        assert archive.testzip() is None
        for name in ['README.md', 'LICENSE', 'CI/brutal-release.json',
                     'apps/openmw/mwrender/brutalblood.cpp', 'components/ragdoll/tests/render_pose.cpp']:
            assert archive.read(m['sourceFile'][:-4] + '/' + name) == git('show', commit + ':' + name)
    (output / 'RELEASE-INFO.json').write_bytes(json_bytes(provenance))
    assets = [runtime, source, output / 'RELEASE-INFO.json']
    checksums = ''.join(sha(path.read_bytes()) + '  ' + path.name + '\n' for path in assets)
    (output / 'SHA256SUMS').write_bytes(checksums.encode('ascii'))
    print('PASS all original executable/library/resource payloads match the current 1.1 build')
    print('PASS runtime/source ZIP integrity and corresponding source revision')
    print(checksums, end='')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--from-report', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.from_report:
        args.output.mkdir(parents=True, exist_ok=True)
        incoming = args.output.parent / MANIFEST['baselineFile']
        with incoming.open('wb') as handle:
            subprocess.run(['git', '-C', str(ROOT), 'show',
                            MANIFEST['baselineReport'] + ':' + MANIFEST['baselineFile']], stdout=handle, check=True)
    else:
        if not args.input:
            parser.error('--input or --from-report is required')
        incoming = args.input.resolve()
    package(incoming, args.output.resolve())
