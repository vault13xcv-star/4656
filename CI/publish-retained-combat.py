"""Publish an unchanged compiled engine with a disclosed test-fixture failure."""
import hashlib
import json
import os
import subprocess
import urllib.request
import zipfile
from pathlib import Path

SOURCE = 'af9a8c9b15aa8a7cf4f8f71445c43717e35d118d'
REPORT = '14daa0cf3eaac0b5ef95d0d4dd5be4ffc6a0e6f1'
VERSION = '1.3-combat.3'
NAME = f'Brutal-Morrowind-{VERSION}-OpenMW-0.51-Windows-x64-{SOURCE[:12]}.zip'
ORIGINAL_HASH = '484f0a24260c11ccf783ab4e3fd3801a2032b112df4eb33576ee2cee49d7f659'
ORIGINAL_BYTES = 49691444
KNOWN = '''Brutal Morrowind 1.3-combat.3 - retained Windows engine

Engine/source commit: af9a8c9b15aa8a7cf4f8f71445c43717e35d118d
Original Windows run: https://github.com/vault13xcv-star/4656/actions/runs/40698202596

The installed executable version and archive integrity checks passed.
5/5 physics/render/combat groups and 1405/1405 component tests passed.
45/46 native integration tests passed, including the held-shield animation test.
The new PhysicalFriendlyFirePolicyUsesBothActorsCombatTargets test raised an
access violation: it creates real AiCombat packages, whose AiPackage base
constructor initializes its reaction timer through World::getPrng(). The
synthetic test supplied an ESMStore and WorldModel but no World service.
The source branch adds a deterministic PRNG-only World fixture. The corrected
full integration test was compiled locally but was not rerun on Windows.
Consequently the full allied-contact integration test remains unverified;
this is not an all-native-tests-pass claim or an in-game playtest.

The engine executable and all original runtime files are retained unchanged.
This file is the only addition to the original Windows package. The original
VALIDATION.txt and logs still record the failed test step. Do not interpret
packaging/publication success as a passing full test run.

Stamina, sprint and RMB behavior still require a Morrowind-data playtest.
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
    nativePassed=45, nativeTotal=46, componentPassed=1405, standaloneGroupsPassed=5,
    engineSha256=engine_hash, retainedArchiveSha256=ORIGINAL_HASH,
    publicationCommit=git('rev-parse', 'HEAD').decode().strip(),
    originalBuild='https://github.com/vault13xcv-star/4656/actions/runs/40698202596')
(dist / 'package.json').write_text(json.dumps(metadata, indent=2) + '\n')
print('PASS retained input SHA256/CRC, unchanged engine and runtime sources, added known-issue notice, source archive')
print(json.dumps(metadata, indent=2))
