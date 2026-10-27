#!/usr/bin/env python3
"""Publish the manifest-selected stable release after verifying its assets."""
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
M = json.loads((ROOT / 'CI/brutal-release.json').read_text(encoding='utf-8'))
DIST = ROOT / 'dist'
REPO = M['repository']
TAG = M['tag']


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def output(*args):
    return subprocess.check_output(args).decode('utf-8-sig')


def release():
    command = ['gh', 'release', 'view', TAG, '--repo', REPO,
               '--json', 'isDraft,isPrerelease,url,tagName,assets']
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        run('gh', 'release', 'create', TAG, '--repo', REPO, '--verify-tag', '--draft',
            '--title', f"Brutal Morrowind {M['version']} — Windows x64",
            '--notes-file', str(ROOT / f'docs/releases/{TAG}.md'))
        data = json.loads(output(*command))
    else:
        data = json.loads(result.stdout)
    files = [M['runtimeFile'], M['sourceFile'], 'RELEASE-INFO.json', 'SHA256SUMS']
    if data['isDraft']:
        run('gh', 'release', 'upload', TAG, '--repo', REPO, '--clobber',
            *(str(DIST / name) for name in files))
    with tempfile.TemporaryDirectory(prefix='brutal-verify-') as temporary:
        run('gh', 'release', 'download', TAG, '--repo', REPO, '--dir', temporary)
        for name in files:
            actual = hashlib.sha256((Path(temporary) / name).read_bytes()).hexdigest()
            expected = hashlib.sha256((DIST / name).read_bytes()).hexdigest()
            if actual != expected:
                raise RuntimeError('GitHub Release asset mismatch: ' + name)
    if data['isDraft']:
        run('gh', 'release', 'edit', TAG, '--repo', REPO, '--draft=false',
            '--prerelease=false', '--latest')
    data = json.loads(output(*command))
    assert not data['isDraft'] and not data['isPrerelease'] and data['tagName'] == TAG
    assert {item['name'] for item in data['assets']} == set(files)
    for attempt in range(6):
        latest = json.loads(output('gh', 'api', 'repos/' + REPO + '/releases/latest'))
        if latest['tag_name'] == TAG and not latest['prerelease'] and not latest['draft']:
            break
        if attempt == 5:
            raise RuntimeError('Published ' + TAG + ' was not returned as the latest stable release')
        time.sleep(2)
    print('PASS published stable Latest release and downloaded asset SHA256 checks')
    return data['url']


def report(result, url, error=None):
    # Git transport makes the publication report accessible to the cloud
    # workspace even where direct GitHub REST access is unavailable.
    branch = 'build-results/brutal-' + TAG
    directory = Path(tempfile.mkdtemp(prefix='brutal-release-report-'))
    token = os.environ['GH_TOKEN']
    def git(*args):
        return run('git', '-C', str(directory), *args)
    git('init', '--initial-branch', branch)
    git('config', 'user.name', 'github-actions[bot]')
    git('config', 'user.email', '41898282+github-actions[bot]@users.noreply.github.com')
    git('config', 'core.autocrlf', 'false')
    auth = base64.b64encode(('x-access-token:' + token).encode()).decode()
    git('config', 'http.https://github.com/.extraheader', 'AUTHORIZATION: basic ' + auth)
    git('remote', 'add', 'origin', 'https://github.com/' + REPO + '.git')
    existing = subprocess.run(['git', '-C', str(directory), 'fetch', '--depth', '1',
                               'origin', 'refs/heads/' + branch], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if existing.returncode == 0:
        git('checkout', '-B', branch, 'FETCH_HEAD')
    status = {
        'result': result, 'version': M['version'], 'tag': TAG, 'releaseUrl': url,
        'sourceCommit': output('git', '-C', str(ROOT), 'rev-parse', 'HEAD').strip(),
        'runUrl': 'https://github.com/' + REPO + '/actions/runs/' + os.environ['GITHUB_RUN_ID'],
        'stableLatest': result == 'success', 'assets': [],
    }
    if error:
        status['error'] = error
    for name in [M['runtimeFile'], M['sourceFile'], 'RELEASE-INFO.json', 'SHA256SUMS']:
        path = DIST / name
        if not path.is_file():
            continue
        status['assets'].append({'file': name, 'bytes': path.stat().st_size,
                                'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        if path.stat().st_size < 95 * 1024 * 1024:
            shutil.copyfile(path, directory / name)
    (directory / 'status.json').write_bytes((json.dumps(status, indent=2) + '\n').encode())
    (directory / 'README.md').write_bytes((
        '# Brutal Morrowind ' + M['version'] + ' publication\n\nResult: **' + result + '**.\n\n'
        '[Release](' + url + ') · [Repository](https://github.com/' + REPO + ')\n\n'
        'status.json records release verification and downloadable file hashes.\n').encode())
    git('add', '--all')
    git('commit', '-m', 'Brutal Morrowind ' + M['version'] + ' publication: ' + result)
    git('push', 'origin', 'HEAD:refs/heads/' + branch)


if __name__ == '__main__':
    if '--report-failure' in sys.argv:
        logs = []
        for name in ['package-release.log', 'installed-version.log', 'installed-version-errors.log']:
            path = ROOT / name
            if path.is_file():
                logs.append(name + ':\n' + path.read_text(encoding='utf-8-sig', errors='replace')[-18000:])
        report('failure', 'https://github.com/' + REPO + '/releases/tag/' + TAG,
               '\n'.join(logs) or 'Publication stopped before the release step; see the Actions run.')
        sys.exit(0)
    try:
        published = release()
    except Exception as error:
        report('failure', 'https://github.com/' + REPO + '/releases/tag/' + TAG,
               type(error).__name__ + ': ' + str(error))
        raise
    report('success', published)
