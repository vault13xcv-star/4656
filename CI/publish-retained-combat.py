"""Publish an unchanged compiled engine with a disclosed test-fixture failure."""
import hashlib
import json
import os
import subprocess
import urllib.request
import zipfile
from pathlib import Path

SOURCE = '184490246889220d99ed94a77ed59e656edc8a89'
REPORT = '0259220c2852d4e1f4c77e91d4a26889241ef5c2'
VERSION = '1.3-combat.2'
NAME = f'Brutal-Morrowind-{VERSION}-OpenMW-0.51-Windows-x64-{SOURCE[:12]}.zip'
ORIGINAL_HASH = '1758e25ba734492534dcef24b4f27c361c486487bc393a3fa002151fddea3a2a'
ORIGINAL_BYTES = 49687910
KNOWN = '''Brutal Morrowind 1.3-combat.2 - retained Windows engine

Engine/source commit: 184490246889220d99ed94a77ed59e656edc8a89
Original Windows run: https://github.com/vault13xcv-star/4656/actions/runs/40658338776

The installed executable version and archive integrity checks passed.
5/5 physics/render/combat groups and 1405/1405 component tests passed.
44/45 native integration tests passed. The new held-shield animation test
raised a Windows access violation because its synthetic environment lacked
the LuaManager required by Animation::animationEnded when a track is replaced.
The running game initializes that manager. The source branch adds the missing LuaManager initialization; that revised
fixture was not executed in this compiled run. No all-native-tests-pass claim is made.

The engine executable and existing runtime files are retained unchanged.
This file is the only addition to the original Windows package. The original
VALIDATION.txt and logs still record the failed test step. Do not interpret
the packaging/publication result as a passing test run.

Actual shield visuals and the guard duel still need a Morrowind-data playtest.
Use a separate installation and copied saves.
'''

def git(*args):
    return subprocess.check_output(['git', *args])

try:
    git('cat-file', '-e', SOURCE + '^{commit}')
except subprocess.CalledProcessError:
    subprocess.run(['git', 'fetch', '--no-tags', '--depth=1', 'origin', SOURCE], check=True)

# Only tests, CI and documentation may differ from the compiled engine.
subprocess.run(['git', 'diff', '--exit-code', SOURCE, 'HEAD', '--',
    'apps/openmw', 'components', 'files'], check=True)

dist = Path('dist-retained')
dist.mkdir(exist_ok=True)
runtime = dist / NAME
cache = os.environ.get('RETAINED_PACKAGE_CACHE')
if cache:
    runtime.write_bytes(Path(cache).read_bytes())
else:
    url = f'https://raw.githubusercontent.com/vault13xcv-star/4656/{REPORT}/{NAME}'
    with urllib.request.urlopen(url, timeout=120) as response, runtime.open('wb') as output:
        while block := response.read(1024 * 1024):
            output.write(block)
assert runtime.stat().st_size == ORIGINAL_BYTES
assert hashlib.sha256(runtime.read_bytes()).hexdigest() == ORIGINAL_HASH
with zipfile.ZipFile(runtime) as archive:
    assert archive.testzip() is None
    executable, = [n for n in archive.namelist() if n.endswith('/openmw.exe')]
    prefix = executable[:-len('openmw.exe')]
    engine_hash = hashlib.sha256(archive.read(executable)).hexdigest()

def add_notice(path, name):
    with zipfile.ZipFile(path, 'a') as archive:
        info = zipfile.ZipInfo(name, (2026, 10, 28, 9, 4, 54))
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, KNOWN)

add_notice(runtime, prefix + 'KNOWN-ISSUES.txt')
with zipfile.ZipFile(runtime) as archive:
    assert archive.testzip() is None
    assert hashlib.sha256(archive.read(executable)).hexdigest() == engine_hash

source = dist / f'Brutal-Morrowind-{VERSION}-Source-{SOURCE[:12]}.zip'
subprocess.run(['git', 'archive', '--format=zip', '--output=' + str(source), SOURCE], check=True)
add_notice(source, 'KNOWN-ISSUES.txt')
with zipfile.ZipFile(source) as archive:
    assert archive.testzip() is None

hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [runtime, source]}
(dist / 'SHA256SUMS').write_text(''.join(f'{digest}  {name}\n' for name, digest in hashes.items()))
metadata = dict(file=NAME, sha256=hashes[NAME], bytes=runtime.stat().st_size,
    sourceCommit=SOURCE, ragdollVersion=VERSION, modName='Brutal Morrowind',
    testResult='failure', validationStatus='known-test-fixture-failure',
    nativePassed=44, nativeTotal=45, componentPassed=1405, standaloneGroupsPassed=5,
    engineSha256=engine_hash, retainedArchiveSha256=ORIGINAL_HASH,
    publicationCommit=git('rev-parse', 'HEAD').decode().strip(),
    originalBuild='https://github.com/vault13xcv-star/4656/actions/runs/40658338776')
(dist / 'package.json').write_text(json.dumps(metadata, indent=2) + '\n')
print('PASS retained input SHA256/CRC, unchanged engine and runtime sources, added known-issue notice, source archive')
print(json.dumps(metadata, indent=2))
