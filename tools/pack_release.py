"""Writes the two release zips from a built clean clone (tools/make_release.sh, D42).

    pack_release.py <clone> <version> <dist>
"""
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import zipfile

CLONE, VERSION, DIST = sys.argv[1], sys.argv[2], sys.argv[3]
NAME = 'MDMI-Mega-Drive-' + VERSION
OUT = os.path.join(CLONE, 'build', 'cmake', 'out')


def git(cwd, *args):
    return subprocess.run(['git', '-C', cwd] + list(args), check=True, capture_output=True).stdout


def lock():
    values = {}
    with open(os.path.join(CLONE, 'tools', 'deps.lock'), encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                key, value = line.split('=', 1)
                values[key] = value
    return values


def add_tree(tar, repo, commit, prefix):
    """Adds the tree of commit in repo, and of its submodules at their recorded commits, under prefix."""
    data = git(repo, 'archive', '--format=tar', commit)
    with tarfile.open(fileobj=io.BytesIO(data)) as src:
        for member in src.getmembers():
            if member.name == 'pax_global_header':
                continue
            member.name = prefix + member.name
            tar.addfile(member, src.extractfile(member) if member.isfile() else None)
    for line in git(repo, 'ls-tree', '-r', commit).decode().splitlines():
        meta, path = line.split('\t', 1)
        mode, kind, sha = meta.split()
        if kind == 'commit':
            add_tree(tar, os.path.join(repo, path), sha, prefix + path + '/')


def dependency_archive(name, commit):
    """The archive tools/fetch_deps.sh unpacks: the pinned commit's tree, submodules included, no .git."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w:gz') as tar:
        add_tree(tar, os.path.join(CLONE, 'third_party', name), commit, '')
    return buffer.getvalue()


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def win64_zip():
    path = os.path.join(DIST, NAME + '-win64.zip')
    files = [
        (os.path.join(OUT, 'VST3', 'MDMI Mega Drive.vst3'), 'MDMI Mega Drive.vst3'),
        (os.path.join(OUT, 'CLAP', 'MDMI Mega Drive.clap'), 'MDMI Mega Drive.clap'),
        (os.path.join(CLONE, 'LICENSE'), 'LICENSE.txt'),
        (os.path.join(CLONE, 'NOTICE'), 'NOTICE.txt'),
        (os.path.join(CLONE, 'THIRD_PARTY_NOTICES.txt'), 'THIRD_PARTY_NOTICES.txt'),
        (os.path.join(CLONE, 'README.md'), 'README.txt'),
    ]
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for source, name in files:
            z.write(source, NAME + '/' + name)
    return path


def source_zip():
    path = os.path.join(DIST, NAME + '-source.zip')
    deps = lock()
    prefix = NAME + '-source/'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        with tarfile.open(fileobj=io.BytesIO(git(CLONE, 'archive', '--format=tar', 'HEAD'))) as tree:
            for member in tree.getmembers():
                if member.isfile():
                    info = zipfile.ZipInfo(prefix + member.name, date_time=(2026, 1, 1, 0, 0, 0))
                    info.external_attr = (member.mode & 0o777) << 16
                    info.compress_type = zipfile.ZIP_DEFLATED
                    z.writestr(info, tree.extractfile(member).read())
        # BlastEm's archive is already in the tree; mdmi's tree leaves out the samples its Makefile fetches.
        for name, key in (('clap', 'CLAP_COMMIT'), ('clap-wrapper', 'CLAP_WRAPPER_COMMIT'),
                          ('vst3sdk', 'VST3SDK_COMMIT'), ('mdmi', 'MDMI_COMMIT')):
            commit = deps[key]
            z.writestr(prefix + 'vendor/%s-%s.tar.gz' % (name, commit[:7]), dependency_archive(name, commit),
                       compress_type=zipfile.ZIP_STORED)
            print('vendor/%s-%s.tar.gz' % (name, commit[:7]))
        samples = os.path.join(CLONE, 'third_party', 'samples.zip')
        archive = os.path.join(CLONE, deps['SAMPLES_ARCHIVE'])
        if not os.path.exists(samples):
            samples = archive
        if sha256(samples) != deps['SAMPLES_SHA256']:
            raise SystemExit('pack_release: %s does not match deps.lock' % samples)
        z.write(samples, prefix + deps['SAMPLES_ARCHIVE'], compress_type=zipfile.ZIP_STORED)
    return path


paths = [win64_zip(), source_zip()]
with open(os.path.join(DIST, NAME + '-SHA256SUMS.txt'), 'w', newline='\n') as f:
    for p in paths:
        line = '%s  %s' % (sha256(p), os.path.basename(p))
        print(line)
        f.write(line + '\n')
