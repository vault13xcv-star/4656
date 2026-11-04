"""Verify and publish the completed G.O.R.E engine without recompiling it."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.request
import zipfile

SOURCE = '31b0943e93241a2b5c7aa4a9ee15b71d5d2f10dc'
REPORT = 'ef6f91fac80dbb26ccb29f8d65433873884727e8'
ORIGINAL_HASH = '22719888ed1fefabc40b6f858365b17e2939c497e18ebe617c9e37020a28ab64'
NAME = 'GORE-Morrowind-1.0-Windows-x64.zip'
ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / 'dist-retained-gore'
BASE = f'https://raw.githubusercontent.com/vault13xcv-star/4656/{REPORT}/'

def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT)

def digest(data):
    return hashlib.sha256(data).hexdigest()

def fetch(name):
    with urllib.request.urlopen(BASE + name, timeout=90) as response:
        return response.read()

head = git('rev-parse', 'HEAD').decode().strip()
# The correction must never substitute different engine/resource sources.
assert not git('diff', SOURCE, head, '--', 'apps/openmw', 'apps/openmw_tests',
               'components', 'files', 'CMakeLists.txt', 'cmake')
status = json.loads(fetch('status.json').decode('utf-8-sig'))
assert status['sourceCommit'] == SOURCE and status['result'] == 'failure'
assert status['package']['sha256'] == ORIGINAL_HASH
groups = fetch('logs/ragdoll-tests.log').decode('utf-8-sig')
tests = fetch('logs/components-tests.log').decode('utf-8-sig')
assert '100% tests passed, 0 tests failed out of 5' in groups
assert re.search(r'\[\s*PASSED\s*\]\s+1404 tests', tests)
assert re.findall(r'^\[\s*FAILED\s*\]\s+(SettingsValuesTest\.\S+)', tests, re.M) == [
    'SettingsValuesTest.physicsLayoutBindsEverySliderToHalfAndTripleItsActualDefault']
for log in ['installed-version.log', 'installed-gore-version.log']:
    assert 'Revision: ' + SOURCE[:10] in fetch('logs/' + log).decode('utf-8-sig')

DIST.mkdir(exist_ok=True)
cache = os.environ.get('RETAINED_PACKAGE_CACHE')
original = Path(cache).read_bytes() if cache else fetch(NAME)
assert len(original) == 49666050 and digest(original) == ORIGINAL_HASH
runtime = DIST / NAME
runtime.write_bytes(original)
with zipfile.ZipFile(runtime) as before:
    assert before.testzip() is None
    executable, = [n for n in before.namelist() if n.endswith('/openmw.exe')]
    prefix = executable[:-len('openmw.exe')]
    entry_hashes = {n: digest(before.read(n)) for n in before.namelist()}
    assert SOURCE in before.read(prefix + 'BUILD-INFO.txt').decode('utf-8-sig')
    assert b'G.O.R.E: Morrowind 1.0 (OpenMW)' in before.read(executable)
    for group in ['blood', 'gore']:
        manifest = json.loads((ROOT / f'CI/brutal-{group}-dds-sha256.json').read_text())
        for name, expected in manifest.items():
            assert entry_hashes[prefix + 'resources/vfs/textures/brutal/' + group + '/' + name] == expected

notice = (ROOT / 'docs/gore-validation.md').read_bytes()
info = dict(modName='G.O.R.E: Morrowind', modVersion='1.0',
    binarySourceCommit=SOURCE, releaseSourceCommit=head, originalBuild=status['runUrl'],
    originalArchiveSha256=ORIGINAL_HASH, engineSha256=entry_hashes[executable],
    runtimeRecompiled=False, originalRuntimeEntriesUnchanged=True,
    standaloneGroupsPassed=5, componentPassed=1404, componentTotal=1405,
    correctedSettingsLinuxPassed=17, correctedSettingsLinuxTotal=17,
    nativeWindowsTestsRun=False, correctedWindowsTestRerun=False,
    validationStatus='known-menu-test-fixture-failure', knownIssues='KNOWN-ISSUES.md')
info_bytes = (json.dumps(info, indent=2) + '\n').encode()
with zipfile.ZipFile(runtime, 'a', compression=zipfile.ZIP_DEFLATED) as after:
    after.writestr(prefix + 'KNOWN-ISSUES.md', notice)
    after.writestr(prefix + 'RELEASE-INFO.json', info_bytes)
with zipfile.ZipFile(runtime) as after:
    assert after.testzip() is None and len(after.namelist()) == len(entry_hashes) + 2
    for name, expected in entry_hashes.items():
        assert digest(after.read(name)) == expected, name

source = DIST / 'GORE-Morrowind-1.0-Source.zip'
subprocess.run(['git', '-c', 'core.autocrlf=false', 'archive', '--format=zip',
                '--output=' + str(source), head], cwd=ROOT, check=True)
(DIST / 'KNOWN-ISSUES.md').write_bytes(notice)
(DIST / 'RELEASE-INFO.json').write_bytes(info_bytes)
(DIST / 'SHA256SUMS').write_text(''.join(digest(p.read_bytes()) + '  ' + p.name + '\n'
    for p in [runtime, source, DIST / 'KNOWN-ISSUES.md', DIST / 'RELEASE-INFO.json']))
metadata = dict(file=NAME, bytes=runtime.stat().st_size, sha256=digest(runtime.read_bytes()),
    sourceCommit=SOURCE, releaseSourceCommit=head, modName=info['modName'], modVersion='1.0',
    ragdollVersion='1.0', testResult='failure', validationStatus=info['validationStatus'],
    componentPassed=1404, componentTotal=1405, standaloneGroupsPassed=5,
    nativeWindowsTestsRun=False, correctedSettingsLinuxPassed=17,
    retainedArchiveSha256=ORIGINAL_HASH, engineSha256=entry_hashes[executable])
(DIST / 'package.json').write_text(json.dumps(metadata, indent=2) + '\n')
print('PASS original archive hash/CRC; every compiled runtime entry unchanged; exception disclosed.')
print(json.dumps(metadata, indent=2))
