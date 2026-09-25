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
p.add_argument('--libdir', default='lib')
p.add_argument('--includedir', default='include')
p.add_argument('--legacy', action='store_true')
a = p.parse_args()
a.source = a.source.resolve(); a.build = a.build.resolve(); a.artifact = a.artifact.resolve()
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
    for h in ('logger.h', 'audit.h', 'console.h'):
        text = re.sub(r'/\*.*?\*/', '', (a.source/'include'/h).read_text(), flags=re.S)
        for line in text.splitlines():
            m = re.match(r'^(?:LOGGER_API\s+)?(?:logger_t\s*\*|logger_state_t|logger_context_t|audit_failure_policy_t|const char\s*\*|uint64_t|int|void)\s*((?:logger_|audit_|console_)\w*)\(', line)
            if m:
                assert line.startswith('LOGGER_API '), line
                declared.add(m.group(1))
    manifest = {n for n in (a.source/'cmake/logger.symbols').read_text().splitlines()
                if n and not n.startswith('#')}
    assert declared == manifest
    stage = work / 'destdir'; env = dict(base_env, DESTDIR=str(stage))
    run([a.cmake, '--install', a.build, '--prefix', '/opt/logger package'], env=env)
    original = stage / 'opt/logger package'
    assert original.is_dir()
    prefix = work / 'relocated prefix'
    original.rename(prefix)
    include = prefix / a.includedir / 'logger'
    lib = prefix / a.libdir
    config_dir = lib / 'cmake/Logger'
    pc_dir = lib / 'pkgconfig'
    expected_headers = {'logger.h','audit.h','console.h','logger_export.h','logger_version.h'}
    if a.legacy: expected_headers.add('logger_fork_compat.h')
    assert {x.name for x in include.iterdir()} == expected_headers
    for x in prefix.rglob('*'):
        if x.is_file() and x.suffix in ('.cmake','.pc'):
            text = x.read_text()
            assert str(a.source) not in text and str(a.build) not in text, x
        assert x.name not in {'logger_internal.h','logger_test_support.a','liblogger_test_support.a',
                              'logger_regression_support.a','liblogger_regression_support.a','logger_fault.h'}
    # The copy is a stand-alone consumer tree, not add_subdirectory(Logger).
    consumer = work / 'external source'
    shutil.copytree(a.source / 'examples/installed_consumer', consumer)
    cb = work / 'external build'
    run([a.cmake, '-S', consumer, '-B', cb, '-DCMAKE_PREFIX_PATH='+str(prefix),
         '-DCMAKE_C_COMPILER='+a.cc, '-DCMAKE_CXX_COMPILER='+a.cxx,
         '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON',
         '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCONSUMER_TEST_CXX=ON'])
    run([a.cmake, '--build', cb, '-j2'])
    commands=(cb/'compile_commands.json').read_text()
    assert str(a.source/'include') not in commands and str(a.source/'src') not in commands
    assert 'LOGGER_ENABLE_FAULT_INJECTION' not in commands
    assert 'LOGGER_ENABLE_LEGACY_FORK_HELPER' not in commands or a.legacy
    for exe,args in [('logger_consumer',[]),('logger_cpp',[]),('plugin_loader',[cb/'libinstalled_plugin.so'])]:
        wd=work/(exe+' run');wd.mkdir()
        run([cb/exe]+args,cwd=wd)
    if a.kind=='static':
        names=run(['nm','-D','--defined-only',cb/'libinstalled_plugin.so'])
        assert 'plugin_run' in names
        assert not any((' logger_' in l or ' audit_' in l or ' console_' in l) for l in names.splitlines())
    # Test pkg-config independently of CMake. Quoted paths must survive spaces.
    pe = dict(base_env, PKG_CONFIG_LIBDIR=str(pc_dir))
    assert run(['pkg-config','--modversion','logger'],env=pe).strip()=='0.9.1'
    opts=['pkg-config','--cflags','--libs']
    if a.kind=='static': opts.append('--static')
    flags=shlex.split(run(opts+['logger'],env=pe))
    exe=work/'pkg consumer'
    run([a.cc,'-std=c11','-Wall','-Wextra','-Wpedantic','-Werror',consumer/'main.c',
         '-o',exe]+flags)
    pe['LD_LIBRARY_PATH']=str(lib)
    wd=work/'pkg run';wd.mkdir();run([exe],cwd=wd,env=pe)
    # Exact candidate version/components fail closed. No guessed compatibility.
    q=work/'query';q.mkdir()
    for version,component,success in [('0.9.1',a.kind,True),('0.9.2',a.kind,False),
        ('1.0.0',a.kind,False),('0.9.1','static' if a.kind=='shared' else 'shared',False),
        ('0.9.1','invented',False),('0.9.1','legacy_fork',a.legacy)]:
        (q/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\nproject(query C)\n'
            'find_package(Logger '+version+' EXACT CONFIG REQUIRED COMPONENTS '+component+')\n')
        run([a.cmake,'-S',q,'-B',work/('query-%d'%counter),'-DLogger_DIR='+str(config_dir)],
            expected=0 if success else 1)
    # Old public headers with the NEW installed lib. Source layout preserved.
    old_include=a.source/'tests/compat/release_input'
    oldexe=work/'old-header-consumer'
    link_flags=['-L'+str(lib),'-llogger','-pthread']
    run([a.cc,'-std=c11','-I'+str(old_include),a.source/'tests/packaging/legacy_consumer.c',
         '-o',oldexe]+link_flags)
    wd=work/'old-header run';wd.mkdir();run([oldexe],cwd=wd,env=pe)
    # All public layout/default snapshots remain identical across old C / new C / C++11.
    layouts=[]
    for compiler,standard,inc,language in [(a.cc,'c11',old_include,'c'),(a.cc,'c11',include,'c'),
                                        (a.cxx,'c++11',include,'c++')]:
        ex=work/('layout-%d'%counter)
        run([compiler,'-std='+standard,'-Wall','-Wextra','-Wpedantic','-Werror','-pedantic-errors',
             '-x',language,'-I'+str(inc),a.source/'tests/packaging/layout.c','-o',ex])
        layouts.append(run([ex]))
    assert layouts[0]==layouts[1]==layouts[2]
    (work/'public-layout.txt').write_text(layouts[0])
    if a.kind=='shared':
        dso=lib/'liblogger.so'
        assert dso.is_symlink() and (lib/'liblogger.so.0').is_symlink()
        assert dso.resolve().name=='liblogger.so.0.9.1'
        argv=[sys.executable,a.source/'scripts/check_release_abi.py',dso]
        if a.legacy: argv.append('--legacy-fork')
        run(argv)
        # A deliberately impossible GLIBC ceiling must cause a failing gate.
        run(argv+['--max-glibc','2.0'],expected=1)
        names=[n for n in (a.source/'cmake/logger.symbols').read_text().splitlines()
               if n and not n.startswith('#')]
        if a.legacy: names.append('logger_fork_reinit')
        loader=work/'abi-loader'
        run([a.cc,'-std=c11',a.source/'tests/packaging/loader.c','-o',loader,'-ldl'])
        run([loader,dso]+names)
    artifact=lib/('liblogger.so' if a.kind=='shared' else 'liblogger.a')
    iso=[sys.executable,a.source/'scripts/check_production_artifact.py',artifact]
    if a.legacy: iso.append('--legacy-fork')
    run(iso)
    report={'passed':True,'kind':a.kind,'legacy':a.legacy,'work':str(work),'commands':log,
            'checks':['DESTDIR install','relocated prefix with spaces','public headers only',
                      'CMake consumer','C++11 consumer','PIC SDK plugin','pkg-config consumer',
                      'version/components rejection','frozen old-header consumer',
                      'C/C++ layouts and defaults','production isolation']}
    (work/'RESULT.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
except BaseException as e:
    report={'passed':False,'error':str(e),'work':str(work),'commands':log}
    (work/'RESULT.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2));raise
