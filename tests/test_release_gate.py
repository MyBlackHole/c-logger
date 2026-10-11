#!/usr/bin/env python3
"""Exercise the actual release workflow shell against isolated local Git fixtures.

Python 3.8+, Bash and Git only. These tests never contact GitHub or run a real gh
command. Structural assertions are deliberately strict; YAML syntax validation
is a separate check (see docs/RELEASE_SAFETY.md).
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = (ROOT / '.github/workflows/release-publish.yml').read_text()
TAG = 'v2.3.4'
VERSION = TAG[1:]
ABI = ('local abi_version = "2"\n'
       'local symbol_version = "LOGGER_2.0"\n'
       'local abi_manifest = "abi/logger-2.0.symbols"\n')


def section(text, header, indent):
    start = text.index(' ' * indent + header + '\n')
    lines = text[start:].splitlines()[1:]
    result = []
    for line in lines:
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        result.append(line)
    return '\n'.join(result)


def script(name):
    step = section(WORKFLOW, '- name: ' + name, 6)
    body = section(step, 'run: |', 8)
    return '\n'.join(line[10:] for line in body.splitlines()) + '\n'


GATE = script('Require an explicit existing release tag')
PIN = script('Verify pinned release source')
PUBLISH = script('Publish or repair GitHub release')


def command(args, cwd, env=None, check=True):
    return subprocess.run(args, cwd=str(cwd), env=env, check=check,
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class Fixture:
    def __init__(self, version=VERSION, abi=ABI, manifest='logger_create\n',
                 annotated=False, tag=True):
        self.temp = tempfile.TemporaryDirectory(prefix='logger-release-gate-')
        self.root = Path(self.temp.name)
        self.repo = self.root / 'source'
        self.repo.mkdir()
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1',
                        GIT_CONFIG_GLOBAL=os.devnull, GIT_AUTHOR_NAME='Release test',
                        GIT_AUTHOR_EMAIL='test@example.invalid',
                        GIT_COMMITTER_NAME='Release test',
                        GIT_COMMITTER_EMAIL='test@example.invalid')
        self.git('init', '-q')
        self.git('checkout', '-qb', 'main')
        (self.repo / 'VERSION').write_text(version + '\n')
        (self.repo / 'xmake.lua').write_text(abi)
        (self.repo / 'abi').mkdir()
        if manifest is not None:
            (self.repo / 'abi/logger-2.0.symbols').write_text(manifest)
        (self.repo / 'docs').mkdir()
        (self.repo / ('docs/RELEASE_NOTES_' + VERSION + '.md')).write_text('Fixture\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'Local release fixture')
        self.sha = self.git('rev-parse', 'HEAD').stdout.strip()
        if tag:
            self.git('tag', *(['-a', '-m', 'Fixture tag'] if annotated else []), TAG)
        self.remote = self.root / 'remote.git'
        self.git('clone', '--bare', '-q', str(self.repo), str(self.remote))
        self.git('remote', 'add', 'origin', str(self.remote))
        self.oid = self.git('rev-parse', 'refs/tags/' + TAG).stdout.strip() if tag else ''
        self.output = self.root / 'output'
        self.env.update(GITHUB_EVENT_NAME='workflow_dispatch', REQUESTED_TAG=TAG,
                        GITHUB_REF_TYPE='branch', GITHUB_REF='refs/heads/main',
                        GITHUB_REF_NAME='main', GITHUB_SHA=self.sha,
                        EVENT_DELETED='false', GITHUB_OUTPUT=str(self.output),
                        RELEASE_TAG=TAG, RELEASE_VERSION=VERSION,
                        SOURCE_SHA=self.sha, SOURCE_TAG_OID=self.oid)
        # A fake gh is the only possible release client even on the positive path.
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        fake = self.bin / 'gh'
        fake.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$GH_TEST_LOG"\nexit 71\n')
        fake.chmod(0o755)
        self.gh_log = self.root / 'gh.log'
        self.env.update(PATH=str(self.bin) + os.pathsep + self.env['PATH'],
                        GH_TEST_LOG=str(self.gh_log))

    def git(self, *args):
        return command(['git'] + list(args), self.repo, self.env)

    def run(self, body, **overrides):
        self.output.write_text('')
        return command(['bash', '-c', body], self.repo,
                       dict(self.env, **overrides), check=False)

    def outputs(self):
        return dict(line.split('=', 1) for line in self.output.read_text().splitlines())

    def move_tag(self, same_commit=False):
        if same_commit:
            self.git('tag', '-fa', TAG, '-m', 'Different tag object, same commit')
        else:
            (self.repo / 'marker').write_text('Moved tag\n')
            self.git('add', 'marker')
            self.git('commit', '-qm', 'Another local commit')
            self.git('tag', '-f', TAG)
        command(['git', '--git-dir=' + str(self.remote), 'fetch', '--force',
                 str(self.repo), 'refs/tags/' + TAG + ':refs/tags/' + TAG],
                self.repo, self.env)
        self.git('checkout', '--detach', self.sha)

    def delete_remote_tag(self):
        command(['git', '--git-dir=' + str(self.remote), 'update-ref', '-d',
                 'refs/tags/' + TAG], self.repo, self.env)

    def publish_script(self, exists=False):
        values = dict(version=VERSION, tag=TAG, notes='docs/RELEASE_NOTES_' + VERSION + '.md',
                      title='Fixture release', prerelease='false', source_sha=self.sha,
                      source_tag_oid=self.oid, release_exists=str(exists).lower())
        return re.sub(r'\$\{\{ needs\.prepare\.outputs\.(\w+) \}\}',
                      lambda match: values[match.group(1)], PUBLISH)

    def assets(self):
        directory = self.repo / 'release-assets'
        directory.mkdir()
        for kind in ('shared', 'static'):
            name = 'prod-c-logger-' + VERSION + '-Linux-x86_64-' + kind + '.tar.gz'
            for suffix in ('', '.sha256'):
                (directory / (name + suffix)).write_text('Fixture only\n')

    def close(self):
        self.temp.cleanup()


class ReleaseGateTests(unittest.TestCase):
    def fixture(self, **kwargs):
        fixture = Fixture(**kwargs)
        self.addCleanup(fixture.close)
        return fixture

    def assert_rejected(self, fixture, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(fixture.outputs(), {})
        self.assertFalse(fixture.gh_log.exists())

    def test_workflow_structure_has_no_automatic_branch_route(self):
        triggers = section(WORKFLOW, 'on:', 0)
        self.assertEqual(re.findall(r'^  (\w+):', triggers, re.M),
                         ['workflow_dispatch', 'push'])
        self.assertEqual(section(triggers, 'push:', 2).strip(), 'tags:\n      - "v2.*"')
        dispatch = section(triggers, 'workflow_dispatch:', 2)
        for required in ('tag:', 'required: true', 'type: string'):
            self.assertIn(required, dispatch)
        prepare = section(WORKFLOW, 'prepare:', 2)
        condition = ' '.join(section(prepare, 'if: >-', 4).split())
        self.assertEqual(condition, "github.event_name == 'workflow_dispatch' || "
                         "(github.event_name == 'push' && github.ref_type == 'tag' && "
                         "startsWith(github.ref, 'refs/tags/v2.') && github.event.deleted == false)")
        self.assertLess(prepare.index('id: gate'), prepare.index('id: version'))
        self.assertLess(prepare.index('id: gate'), prepare.index('gh release'))
        self.assertIn('authorized: ${{ steps.gate.outputs.authorized }}', prepare)
        for name in ('package', 'consume-modern', 'publish', 'release-already-exists'):
            job = section(WORKFLOW, name + ':', 2)
            condition = section(job, 'if: >-', 4)
            self.assertIn("needs.prepare.result == 'success'", condition)
            self.assertIn("needs.prepare.outputs.authorized == 'true'", condition)
            self.assertNotIn('always()', condition)
            dependencies = {'publish': '[prepare, package, consume-modern]',
                            'consume-modern': '[prepare, package]'}
            self.assertIn('needs: ' + dependencies.get(name, 'prepare'), job)
            if name != 'release-already-exists':
                self.assertIn('ref: ${{ needs.prepare.outputs.source_sha }}', job)
            if name == 'publish':
                self.assertIn("needs.package.result == 'success'", condition)
                self.assertIn("needs.consume-modern.result == 'success'", condition)
        self.assertNotIn('source_ref', WORKFLOW)
        self.assertNotIn('continue-on-error:', WORKFLOW)
        self.assertNotIn('--target', WORKFLOW)
        self.assertEqual(len(re.findall(r'gh release create ', WORKFLOW)), 1)
        self.assertIn('gh release create "$tag" --verify-tag', PUBLISH)
        self.assertNotRegex(WORKFLOW, r'\bgit (?:tag|push)\b')
        self.assertNotRegex(WORKFLOW, r'gh api.*(?:git/refs|/git/tags)')
        self.assertIn('".github/workflows/release-publish.yml"',
                      (ROOT / '.github/workflows/core-validation.yml').read_text())

    def test_release_packages_and_final_consumers_use_supported_baseline(self):
        for name in ('release-validation', 'release-publish'):
            with self.subTest(workflow=name):
                workflow = (ROOT / '.github/workflows' / (name + '.yml')).read_text()
                package = section(workflow, 'package:', 2)
                self.assertIn('image: ubuntu:20.04', section(package, 'container:', 4))
                self.assertLess(package.index('apt-get install'), package.index('actions/checkout@'))
                self.assertIn('--toolchain=gcc', package)
                self.assertIn("= 'glibc 2.31'", package)
                self.assertIn('f|config|test|install|run|pack)', package)
                self.assertIn('xmake test -j1', package)
                self.assertIn('xmake pack -f targz', package)
                install = section(package, '- name: Verify packaged install tree', 6)
                self.assertIn('--installed-prefix "$prefix"', install)
                self.assertIn('--max-glibc 2.31', install)
                self.assertIn('--max-consumer-glibc 2.31', install)
                modern = section(workflow, 'consume-modern:', 2)
                self.assertIn('runs-on: ubuntu-24.04', modern)
                self.assertIn('actions/download-artifact@', modern)
                self.assertIn('--installed-prefix "$prefix" --max-glibc 2.31', modern)
                self.assertNotIn('--max-consumer-glibc', modern)
                self.assertNotIn('xmake f ', modern)

    def test_every_multiline_shell_parses(self):
        blocks = re.findall(r'^        run: ([|]|>-)\n((?:          .*\n|\n)+)',
                            WORKFLOW, re.M)
        self.assertGreater(len(blocks), 10)
        for index, (style, text) in enumerate(blocks):
            body = '\n'.join(line[10:] for line in text.splitlines())
            if style == '>-':
                body = ' '.join(body.splitlines())
            body = re.sub(r'\$\{\{.*?\}\}', 'fixture', body)
            result = command(['bash', '-n', '-c', body], ROOT, check=False)
            self.assertEqual(result.returncode, 0, 'block %d: %s' % (index, result.stderr))

    def test_rejects_non_release_events(self):
        fixture = self.fixture()
        cases = [dict(GITHUB_EVENT_NAME=event) for event in
                 ('pull_request', 'schedule', 'release', 'repository_dispatch', '')]
        cases += [dict(GITHUB_EVENT_NAME='push'),  # Any main file or workflow merge.
                  dict(GITHUB_EVENT_NAME='push', GITHUB_REF_TYPE='tag',
                       GITHUB_REF_NAME=TAG, GITHUB_REF='refs/tags/' + TAG, EVENT_DELETED='true'),
                  dict(GITHUB_EVENT_NAME='push', GITHUB_REF_TYPE='tag',
                       GITHUB_REF_NAME=TAG, GITHUB_REF='refs/tags/' + TAG, GITHUB_SHA='0' * 40)]
        for case in cases:
            with self.subTest(case=case):
                self.assert_rejected(fixture, fixture.run(GATE, **case))

    def test_rejects_invalid_tag_or_branch_input(self):
        fixture = self.fixture()
        for tag in ('', 'main', 'refs/tags/' + TAG, 'v1.0.0', 'v3.0.0', 'v2.3',
                    'v2.03.4', 'v2.3.04', 'v2.3.4-rc1', 'v2.3.4\nx=1',
                    'v2.3.4; touch injected'):
            with self.subTest(tag=tag):
                self.assert_rejected(fixture, fixture.run(GATE, REQUESTED_TAG=tag))
        self.assertFalse((fixture.repo / 'injected').exists())

    def test_rejects_missing_and_stale_local_only_tags(self):
        fixture = self.fixture(tag=False)
        self.assert_rejected(fixture, fixture.run(GATE))
        fixture = self.fixture()
        fixture.delete_remote_tag()
        self.assert_rejected(fixture, fixture.run(GATE))

    def test_rejects_tag_not_pointing_to_commit(self):
        fixture = self.fixture()
        blob = fixture.git('rev-parse', 'HEAD:VERSION').stdout.strip()
        command(['git', '--git-dir=' + str(fixture.remote), 'update-ref',
                 'refs/tags/' + TAG, blob], fixture.repo, fixture.env)
        self.assert_rejected(fixture, fixture.run(GATE))

    def test_rejects_version_and_abi_contract_mismatches(self):
        cases = [dict(version='1.0.0'), dict(version='2.3.5'),
                 dict(abi=ABI.replace('abi_version = "2"', 'abi_version = "1"')),
                 dict(abi=ABI.replace('LOGGER_2.0', 'LOGGER_1.0')),
                 dict(abi=ABI.replace('logger-2.0.symbols', 'logger-1.0.symbols')),
                 dict(manifest=None), dict(manifest=''), dict(manifest='# empty\n')]
        for case in cases:
            with self.subTest(case=case):
                fixture = self.fixture(**case)
                self.assert_rejected(fixture, fixture.run(GATE))

    def test_existing_lightweight_and_annotated_tags_succeed(self):
        for annotated in (False, True):
            for event in ('workflow_dispatch', 'push'):
                with self.subTest(annotated=annotated, event=event):
                    fixture = self.fixture(annotated=annotated)
                    result = fixture.run(GATE, GITHUB_EVENT_NAME=event,
                                         GITHUB_REF_TYPE='tag', GITHUB_REF_NAME=TAG,
                                         GITHUB_REF='refs/tags/' + TAG)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(fixture.outputs(), dict(authorized='true',
                                     source_sha=fixture.sha, source_tag_oid=fixture.oid))
                    self.assertEqual(fixture.git('rev-parse', 'HEAD').stdout.strip(), fixture.sha)
                    self.assertFalse(fixture.gh_log.exists())

    def test_dispatch_reads_tag_version_instead_of_branch_version(self):
        fixture = self.fixture()
        (fixture.repo / 'VERSION').write_text('1.0.0\n')
        fixture.git('add', 'VERSION')
        fixture.git('commit', '-qm', 'Dispatch branch has another version')
        result = fixture.run(GATE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((fixture.repo / 'VERSION').read_text().strip(), VERSION)
        self.assertEqual(fixture.outputs()['source_sha'], fixture.sha)

    def test_build_rechecks_pin_and_remote_tag_identity(self):
        fixture = self.fixture()
        self.assertEqual(fixture.run(PIN).returncode, 0)
        for overrides in (dict(SOURCE_SHA='0' * 40), dict(SOURCE_TAG_OID='0' * 40),
                          dict(RELEASE_VERSION='2.0.0'), dict(RELEASE_TAG='main')):
            self.assert_rejected(fixture, fixture.run(PIN, **overrides))
        for mode in ('deleted', 'moved', 'retagged'):
            with self.subTest(mode=mode):
                fixture = self.fixture()
                if mode == 'deleted':
                    fixture.delete_remote_tag()
                else:
                    fixture.move_tag(same_commit=mode == 'retagged')
                self.assert_rejected(fixture, fixture.run(PIN))

    def test_publish_rejects_changed_tags_before_any_gh_command(self):
        for exists in (False, True):
            for mode in ('deleted', 'moved', 'retagged'):
                with self.subTest(exists=exists, mode=mode):
                    fixture = self.fixture()
                    fixture.assets()
                    if mode == 'deleted':
                        fixture.delete_remote_tag()
                    else:
                        fixture.move_tag(same_commit=mode == 'retagged')
                    self.assert_rejected(fixture, fixture.run(fixture.publish_script(exists)))

    def test_publish_rejects_wrong_checkout_and_missing_assets(self):
        fixture = self.fixture()
        self.assert_rejected(fixture, fixture.run(fixture.publish_script()))
        fixture.assets()
        (fixture.repo / 'marker').write_text('wrong checkout\n')
        fixture.git('add', 'marker')
        fixture.git('commit', '-qm', 'Wrong checked out commit')
        self.assert_rejected(fixture, fixture.run(fixture.publish_script()))

    def test_publish_positive_path_reaches_only_mocked_release_client(self):
        for exists in (False, True):
            fixture = self.fixture()
            fixture.assets()
            result = fixture.run(fixture.publish_script(exists))
            # Stop at the first fake gh call. No publish workflow or release is run.
            self.assertEqual(result.returncode, 71, result.stderr)
            logged = fixture.gh_log.read_text()
            self.assertIn('release ' + ('edit' if exists else 'create') + ' ' + TAG, logged)
            if not exists:
                self.assertIn('--verify-tag', logged)
                self.assertNotIn('--target', logged)


if __name__ == '__main__':
    unittest.main(verbosity=2)
