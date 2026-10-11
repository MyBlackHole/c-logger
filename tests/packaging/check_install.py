#!/usr/bin/env python3
"""Real install/relocation/consumer checks for one native production build.
Uses only installed headers/libraries for external consumers. Does not assert
old-kernel compatibility or cross-platform ABI compatibility from a native run.
"""
import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--artifact', type=Path, required=True)
p.add_argument('--kind', choices=['shared','static'], required=True)
p.add_argument('--cc', required=True)
p.add_argument('--cxx', required=True)
p.add_argument('--cmake', default='cmake')
p.add_argument('--xmake', default='xmake')
p.add_argument('--installed-prefix', type=Path,
               help='Validate an already installed/extracted prefix instead of running an installer')
p.add_argument('--libdir', default='lib')
p.add_argument('--includedir', default='include')
p.add_argument('--abi-version', default='2')
p.add_argument('--symbol-version', default='LOGGER_2.0')
p.add_argument('--abi-manifest', default='abi/logger-2.0.symbols')
a = p.parse_args()
a.source = a.source.resolve(); a.build = a.build.resolve(); a.artifact = a.artifact.resolve()
version = (a.source / 'VERSION').read_text(encoding='utf-8').strip()
assert re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version), version
assert re.fullmatch(r'[0-9]+', a.abi_version), a.abi_version
assert re.fullmatch(r'LOGGER_[0-9]+\.[0-9]+', a.symbol_version), a.symbol_version
abi_manifest = a.source / a.abi_manifest
assert abi_manifest.is_file(), abi_manifest
version_major, version_minor, version_patch = map(int, version.split('.'))
expected_release_candidate = version_major == 0
next_patch = f'{version_major}.{version_minor}.{version_patch + 1}'
next_major = f'{version_major + 1}.0.0'
work = Path(tempfile.mkdtemp(prefix='install-check-', dir=Path.cwd()))
log = []; counter = 0
base_env = dict(os.environ)
# These must not redirect discovery back to an unrelated installation.
for k in ('Logger_DIR','CMAKE_PREFIX_PATH','PKG_CONFIG_PATH','PKG_CONFIG_LIBDIR',
          'PKG_CONFIG_SYSROOT_DIR','DESTDIR','LD_LIBRARY_PATH'):
    base_env.pop(k, None)

def run(command, cwd=work, env=None, expected=0):
    global counter
    counter += 1
    cmd = [str(x) for x in command]
    cp = subprocess.run(cmd, cwd=cwd, env=env or base_env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    (work / ('%03d.log' % counter)).write_text('$ ' + shlex.join(cmd) + '\n' + cp.stdout)
    log.append({'command': cmd, 'rc': cp.returncode, 'log':'%03d.log' % counter})
    if (expected == 0 and cp.returncode != 0) or (expected != 0 and cp.returncode == 0):
        raise RuntimeError('Unexpected rc=%d (%s)\n%s' % (cp.returncode, shlex.join(cmd), cp.stdout[-6000:]))
    return cp.stdout

try:
    # A newly declared public entry must not silently disappear into local:*.
    declared = set()
    for h in ('logger.h', 'console.h'):
        text = re.sub(r'/\*.*?\*/', '', (a.source/'include'/h).read_text(), flags=re.S)
        for line in text.splitlines():
            m = re.match(r'^(?:LOGGER_API\s+)?(?:logger_t\s*\*|logger_state_t|logger_context_t|audit_failure_policy_t|const char\s*\*|uint64_t|int|void)\s*((?:logger_|audit_|console_)\w*)\(', line)
            if m:
                assert line.startswith('LOGGER_API '), line
                declared.add(m.group(1))
    manifest = {n for n in abi_manifest.read_text().splitlines()
                if n and not n.startswith('#')}
    assert declared == manifest
    stage = work / 'destdir'
    prefix = work / 'relocated prefix'
    if a.installed_prefix:
        original = a.installed_prefix.resolve()
        assert original.is_dir()
        shutil.copytree(original, prefix, symlinks=True)
    else:
        original = stage / 'opt/logger package'
        # Xmake exposes an install root directly with -o. Use a staged nested
        # prefix so the same relocation and external-consumer checks apply.
        run([a.xmake, 'install', '-o', original, 'logger'], cwd=a.source)
        assert original.is_dir()
        original.rename(prefix)
    include = prefix / a.includedir / 'logger'
    lib = prefix / a.libdir
    config_dir = lib / 'cmake/Logger'
    pc_dir = lib / 'pkgconfig'
    license_file = prefix / 'share/doc/prod_c_logger/LICENSE'
    assert license_file.is_file()
    license_text = license_file.read_text()
    assert 'Apache License' in license_text
    assert 'Version 2.0, January 2004' in license_text
    expected_headers = {'logger.h','console.h','logger_export.h','logger_version.h'}
    assert {x.name for x in include.iterdir()} == expected_headers
    version_header = (include / 'logger_version.h').read_text()
    assert re.search(r'^#define LOGGER_ABI_VERSION\s+' + re.escape(a.abi_version) + r'\s*$', version_header, re.M)
    expected_candidate_macro = '1' if expected_release_candidate else '0'
    assert re.search(r'^#define LOGGER_RELEASE_CANDIDATE\s+' + expected_candidate_macro + r'\s*$', version_header, re.M)
    cmake_config = (config_dir / 'LoggerConfig.cmake').read_text()
    expected_candidate_cmake = 'TRUE' if expected_release_candidate else 'FALSE'
    assert re.search(r'^set\(Logger_RELEASE_CANDIDATE\s+' + expected_candidate_cmake + r'\)$', cmake_config, re.M)
    for x in prefix.rglob('*'):
        if x.is_file() and x.suffix in ('.cmake','.pc'):
            text = x.read_text()
            assert str(a.source) not in text and str(a.build) not in text, x
            assert "LOGGER_ENABLE_LEGACY_FORK_HELPER" not in text, x
            assert "logger_fork_compat" not in text, x
        assert x.name not in {'logger_internal.h','logger_test_support.a','liblogger_test_support.a',
                              'logger_regression_support.a','liblogger_regression_support.a','logger_fault.h'}
    # The copy is a stand-alone consumer tree, not add_subdirectory(Logger).
    consumer = work / 'external source'
    shutil.copytree(a.source / 'examples/installed_consumer', consumer)
    cb = work / 'external build'
    run([a.cmake, '-S', consumer, '-B', cb, '-DCMAKE_PREFIX_PATH='+str(prefix),
         '-DCMAKE_C_COMPILER='+a.cc, '-DCMAKE_CXX_COMPILER='+a.cxx,
         '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON',
         '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCONSUMER_TEST_CXX=ON',
         '-DCONSUMER_LOGGER_VERSION='+version])
    run([a.cmake, '--build', cb, '-j2'])
    commands=(cb/'compile_commands.json').read_text()
    assert str(a.source/'include') not in commands and str(a.source/'src') not in commands
    assert 'LOGGER_ENABLE_FAULT_INJECTION' not in commands
    assert 'LOGGER_ENABLE_LEGACY_FORK_HELPER' not in commands
    for exe,args in [('logger_consumer',[]),('logger_cpp',[]),('plugin_loader',[cb/'libinstalled_plugin.so'])]:
        wd=work/(exe+' run');wd.mkdir()
        run([cb/exe]+args,cwd=wd)
    if a.kind=='static':
        names=run(['nm','-D','--defined-only',cb/'libinstalled_plugin.so'])
        assert 'plugin_run' in names
        assert not any((' logger_' in l or ' audit_' in l or ' console_' in l) for l in names.splitlines())
    # Test pkg-config independently of CMake. The CMake consumer above
    # deliberately validates a relocated prefix containing spaces. Older
    # pkg-config implementations do not consistently preserve spaces in
    # pcfiledir-derived -I/-L flags, so validate the .pc contract from a second
    # no-space relocated copy instead of pretending that tool limitation is a
    # package guarantee.
    pkg_prefix=work/'pkg-relocated'
    shutil.copytree(prefix,pkg_prefix,symlinks=True)
    pkg_pc_dir=pkg_prefix/a.libdir/'pkgconfig'
    pkg_lib=pkg_prefix/a.libdir
    pe = dict(base_env, PKG_CONFIG_LIBDIR=str(pkg_pc_dir))
    assert run(['pkg-config','--modversion','logger'],env=pe).strip()==version
    opts=['pkg-config','--cflags','--libs']
    if a.kind=='static': opts.append('--static')
    flags=shlex.split(run(opts+['logger'],env=pe))
    exe=work/'pkg consumer'
    run([a.cc,'-std=c11','-Wall','-Wextra','-Wpedantic','-Werror',consumer/'main.c',
         '-o',exe]+flags)
    pe['LD_LIBRARY_PATH']=str(pkg_lib)
    wd=work/'pkg run';wd.mkdir();run([exe],cwd=wd,env=pe)
    # Exact candidate version/components fail closed. No guessed compatibility.
    q=work/'query';q.mkdir()
    for requested,component,success in [(version,a.kind,True),(next_patch,a.kind,False),
        (next_major,a.kind,False),(version,'static' if a.kind=='shared' else 'shared',False),
        (version,'invented',False),(version,'legacy_fork',False)]:
        (q/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(query C)\n'
            'find_package(Logger '+requested+' EXACT CONFIG REQUIRED COMPONENTS '+component+')\n')
        run([a.cmake,'-S',q,'-B',work/('query-%d'%counter),'-DLogger_DIR='+str(config_dir)],
            expected=0 if success else 1)
    # Freeze the current ABI 2 source layout across C and C++.
    layouts=[]
    for compiler,standard,language in [(a.cc,'c11','c'),(a.cxx,'c++11','c++')]:
        ex=work/('layout-%d'%counter)
        run([compiler,'-std='+standard,'-Wall','-Wextra','-Wpedantic','-Werror','-pedantic-errors',
             '-x',language,'-I'+str(include),a.source/'tests/packaging/layout.c','-o',ex])
        layouts.append(run([ex]))
    assert layouts[0]==layouts[1]
    (work/'public-layout.txt').write_text(layouts[0])
    if a.kind=='shared':
        dso=lib/'liblogger.so'
        soname_link = lib / ('liblogger.so.' + a.abi_version)
        assert dso.is_symlink() and soname_link.is_symlink()
        assert dso.resolve().name=='liblogger.so.'+version
        assert Path(os.readlink(dso)).name == soname_link.name
        argv=[sys.executable,a.source/'scripts/check_release_abi.py',dso,
              '--manifest',abi_manifest,
              '--abi-version',a.abi_version,
              '--symbol-version',a.symbol_version]
        run(argv)
        # A deliberately impossible GLIBC ceiling must cause a failing gate.
        run(argv+['--max-glibc','2.0'],expected=1)
        names=[n for n in abi_manifest.read_text().splitlines()
               if n and not n.startswith('#')]
        loader=work/'abi-loader'
        run([a.cc,'-std=c11',a.source/'tests/packaging/loader.c','-o',loader,'-ldl'])
        run([loader,dso,a.symbol_version]+names)
    artifact=lib/('liblogger.so' if a.kind=='shared' else 'liblogger.a')
    iso=[sys.executable,a.source/'scripts/check_production_artifact.py',artifact]
    run(iso)
    if a.installed_prefix:
        install_check = 'preinstalled package tree'
    else:
        install_check = 'Xmake staged install'
    report={'passed':True,'kind':a.kind,'abi_version':a.abi_version,
            'symbol_version':a.symbol_version,'abi_manifest':str(abi_manifest),
            'install_driver':'xmake',
            'work':str(work),'commands':log,
            'checks':[install_check,'relocated prefix with spaces','public headers only',
                      'CMake consumer','C++11 consumer','PIC SDK plugin','pkg-config consumer',
                      'version/components rejection','current C/C++ layouts and defaults','production isolation']}
    (work/'RESULT.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
except BaseException as e:
    report={'passed':False,'error':str(e),'work':str(work),'commands':log}
    (work/'RESULT.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2));raise
