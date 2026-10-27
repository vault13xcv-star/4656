#!/usr/bin/env python3
"""Publish the pinned 1.1 candidate with its obsolete-test exception disclosed.

No engine compilation, test-result substitution or stable-release promotion.
"""
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
M = json.loads((ROOT / 'CI/brutal11-retained.json').read_text())
DIST = ROOT / 'dist'
REPO = 'vault13xcv-star/4656'


def output(*args):
    return subprocess.check_output(args, cwd=ROOT)


def run(*args):
    subprocess.run(args, cwd=ROOT, check=True)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def show(path):
    return output('git', 'show', M['reportCommit'] + ':' + path)


def prepare():
    head = output('git', 'rev-parse', 'HEAD').decode().strip()
    # Only tests, release documentation and publication machinery may differ.
    assert not output('git', 'diff', M['binarySource'], head, '--',
                      'apps/openmw', 'components', 'files', 'CMakeLists.txt', 'cmake')
    status = json.loads(show('status.json').decode('utf-8-sig'))
    assert status['stage'] == 'completed' and status['result'] == 'failure'
    assert status['sourceCommit'] == M['binarySource']
    assert status['package']['sha256'] == M['sha256']
    logs = {}
    for name in ['ragdoll-tests.log', 'components-tests.log', 'item-integration-tests.log']:
        logs[name] = show('logs/' + name)
    assert '100% tests passed, 0 tests failed out of 4' in logs['ragdoll-tests.log'].decode('utf-8-sig')
    assert re.search(r'\[\s*PASSED\s*\]\s+1402 tests', logs['components-tests.log'].decode('utf-8-sig'))
    integration = logs['item-integration-tests.log'].decode('utf-8-sig')
    failed = re.findall(r'^\[\s*FAILED\s*\]\s+(ItemPhysics\S+)', integration, re.M)
    expected = 'ItemPhysicsIntegration.NativeWeaponClassesAndAnimationKeysSelectOnlyEligibleWounds'
    assert failed == [expected, expected], failed  # failed case + final summary
    assert re.search(r'\[\s*FAILED\s*\]\s+1 test,', integration)
    assert re.search(r'\[\s*PASSED\s*\]\s+38 tests', integration)
    assert integration.count('torso.cuts(1)') == 2
    assert integration.count('Which is: false') == integration.count('Which is: true') == 2
    original = show(M['runtimeFile'])
    assert len(original) == M['bytes'] and digest(original) == M['sha256']
    DIST.mkdir(exist_ok=True)
    notes = (ROOT / 'docs/releases/v1.1-experimental.2-known-issues.md').read_bytes()
    info = {
        'version': M['version'], 'binarySourceCommit': M['binarySource'],
        'correspondingSourceCommit': head, 'compiledCandidateRun': status['runUrl'],
        'originalArchiveSha256': M['sha256'], 'originalWindowsTestResult': 'failure',
        'physicsRenderGroupsPassed': 4, 'componentTestsPassed': 1402,
        'nativeIntegrationTestsPassed': 38, 'nativeIntegrationTestsTotal': 39,
        'exception': expected + ' expected the intentionally removed hammer waist cut',
        'correctedWindowsTestRerun': False, 'runtimeRecompiled': False,
        'engineAndResourceSourcesIdentical': True,
        'originalRuntimeEntriesUnchanged': True,
        'knownIssues': 'KNOWN-ISSUES.md',
    }
    info_bytes = (json.dumps(info, indent=2) + '\n').encode()
    target = DIST / M['runtimeFile']
    with zipfile.ZipFile(io.BytesIO(original)) as before, zipfile.ZipFile(target, 'w') as after:
        assert before.testzip() is None
        exe = [n for n in before.namelist() if n.endswith('/openmw.exe')]
        assert len(exe) == 1
        prefix = exe[0][:-len('openmw.exe')]
        assert M['binarySource'] in before.read(prefix + 'BUILD-INFO.txt').decode('utf-8-sig')
        for group in ['gore', 'blood']:
            manifest = json.loads((ROOT / f'CI/brutal-{group}-dds-sha256.json').read_text())
            for name, expected_hash in manifest.items():
                assert digest(before.read(prefix + 'resources/vfs/textures/brutal/' + group + '/' + name)) == expected_hash
        for entry in before.infolist():
            after.writestr(entry, before.read(entry.filename))
        after.writestr(prefix + 'KNOWN-ISSUES.md', notes)
        after.writestr(prefix + 'RELEASE-INFO.json', info_bytes)
    with zipfile.ZipFile(io.BytesIO(original)) as before, zipfile.ZipFile(target) as after:
        assert after.testzip() is None
        assert len(after.namelist()) == len(before.namelist()) + 2
        for name in before.namelist():
            assert before.read(name) == after.read(name), name
    source = f"Brutal-Morrowind-{M['version']}-Source-{head[:12]}.zip"
    run('git', '-c', 'core.autocrlf=false', 'archive', '--format=zip', '--output=' + str(DIST / source), head)
    (DIST / 'KNOWN-ISSUES.md').write_bytes(notes)
    (DIST / 'RELEASE-INFO.json').write_bytes(info_bytes)
    assets = [M['runtimeFile'], source, 'KNOWN-ISSUES.md', 'RELEASE-INFO.json']
    (DIST / 'SHA256SUMS').write_text(''.join(digest((DIST / name).read_bytes()) + '  ' + name + '\n' for name in assets))
    metadata = {
        'file': M['runtimeFile'], 'sha256': digest(target.read_bytes()), 'bytes': target.stat().st_size,
        'sourceCommit': M['binarySource'], 'releaseSourceCommit': head, 'sourceFile': source,
        'ragdollVersion': M['version'], 'modName': 'Brutal Morrowind',
        'testResult': '38/39 native integration; one obsolete hammer-waist expectation; see KNOWN-ISSUES.md',
    }
    (DIST / 'package.json').write_text(json.dumps(metadata, indent=2))
    log_dir = ROOT / 'ragdoll-logs'
    log_dir.mkdir(exist_ok=True)
    for name, data in logs.items():
        (log_dir / name).write_bytes(data)
    print('Verified all original runtime entries unchanged; only known-issue/provenance files added.')
    print(json.dumps(metadata, indent=2))


def publish():
    head = output('git', 'rev-parse', 'HEAD').decode().strip()
    metadata = json.loads((DIST / 'package.json').read_text())
    assert metadata['releaseSourceCommit'] == head
    command = ['gh', 'release', 'view', M['tag'], '--repo', REPO,
               '--json', 'isDraft,isPrerelease,targetCommitish,url']
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    notes = DIST / 'release-notes.md'
    notes.write_bytes((ROOT / 'docs/releases/v1.1-experimental.2.md').read_bytes()
                      + b'\n\n' + (DIST / 'KNOWN-ISSUES.md').read_bytes())
    if result.returncode:
        run('gh', 'release', 'create', M['tag'], '--repo', REPO, '--target', head,
            '--draft', '--prerelease', '--title', 'Brutal Morrowind 1.1 Experimental 2 - Windows x64',
            '--notes-file', str(notes))
        release = json.loads(output(*command))
    else:
        release = json.loads(result.stdout)
    assets = [M['runtimeFile'], metadata['sourceFile'], 'KNOWN-ISSUES.md', 'RELEASE-INFO.json', 'SHA256SUMS']
    if release['isDraft']:
        run('gh', 'release', 'upload', M['tag'], '--repo', REPO, '--clobber',
            *(str(DIST / name) for name in assets))
    with tempfile.TemporaryDirectory() as temporary:
        run('gh', 'release', 'download', M['tag'], '--repo', REPO, '--dir', temporary)
        for name in assets:
            assert digest((Path(temporary) / name).read_bytes()) == digest((DIST / name).read_bytes()), name
    if release['isDraft']:
        run('gh', 'release', 'edit', M['tag'], '--repo', REPO, '--draft=false', '--prerelease=true', '--latest=false')
    release = json.loads(output(*command))
    assert not release['isDraft'] and release['isPrerelease']
    assert output('gh', 'api', 'repos/' + REPO + '/releases/latest', '--jq', '.tag_name').decode().strip() == 'v1.0'
    (DIST / 'release-url.txt').write_text(release['url'])
    print('Published verified prerelease; stable V1.0 remains Latest.')


if __name__ == '__main__':
    if sys.argv[1:] == ['prepare']:
        prepare()
    elif sys.argv[1:] == ['publish']:
        publish()
    else:
        raise SystemExit('Use prepare or publish')
