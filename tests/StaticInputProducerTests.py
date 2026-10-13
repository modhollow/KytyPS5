"""Exercise the real producer CMake block through real Git checkout policies.

The miniature configure uses LANGUAGES NONE: it hashes the selected real source
inputs without compiling the emulator or replacing the producer algorithm.
"""

import argparse
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile


def run(*args, cwd=None):
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=120)
    if result.returncode:
        raise AssertionError(f"Command failed ({result.returncode}): {args}\n{result.stdout}")
    return result.stdout


def wrapper(block, source=None):
    directory = source.as_posix() if source else '${CMAKE_CURRENT_SOURCE_DIR}/src'
    return ("cmake_minimum_required(VERSION 3.18)\n"
            "project(StaticProducerCheckoutTest LANGUAGES NONE)\n"
            f'set(KYTY_SOURCE_DIR "{directory}")\n' + block +
            '\nfile(WRITE "${PROJECT_BINARY_DIR}/producer-inputs.txt" "")\n'
            'foreach(input IN LISTS KYTY_STATIC_PRODUCER_INPUTS)\n'
            '  file(APPEND "${PROJECT_BINARY_DIR}/producer-inputs.txt" "${input}\\n")\n'
            'endforeach()\n')


def configure(source, build, cmake):
    run(cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja')
    header = (build / 'staticInputProducer.h').read_text()
    match = re.search(r'StaticInputProducer\[\] = "([0-9a-f]{64})"', header)
    assert match, 'Actual CMake configure did not produce a valid identity'
    inputs = (build / 'producer-inputs.txt').read_text().splitlines()
    return match.group(1), [Path(path) for path in inputs]


def checkout(seed, destination, autocrlf, git):
    run(git, 'clone', '--quiet', '--no-hardlinks', '--no-checkout', str(seed), str(destination))
    run(git, '-C', str(destination), 'config', 'core.autocrlf', autocrlf)
    run(git, '-C', str(destination), 'checkout', '--quiet', '--force', 'HEAD')


def check(args, fixture):
    cmake_source = (args.source / 'CMakeLists.txt').read_text()
    start = cmake_source.index('file(GLOB_RECURSE KYTY_STATIC_RECOMPILER_INPUTS')
    end = cmake_source.index('\nfind_package(Git)', start)
    block = cmake_source[start:end]
    assert 'SHA256' in block and 'KYTY_STATIC_INPUT_HASH' in block and 'staticInputProducer.h' in block

    discovery = fixture / 'discovery'
    discovery.mkdir()
    (discovery / 'CMakeLists.txt').write_text(wrapper(block, args.source / 'src'))
    current, selected = configure(discovery, fixture / 'discovery-build', args.cmake)
    assert selected, 'Producer input selection is empty'

    seed = fixture / 'seed'
    seed.mkdir()
    run(args.git, '-c', 'init.defaultBranch=producer-test', 'init', '--quiet', str(seed))
    (seed / '.gitattributes').write_bytes((args.source / '.gitattributes').read_bytes())
    (seed / 'CMakeLists.txt').write_text(wrapper(block))
    relative_inputs = []
    for path in selected:
        relative = path.resolve().relative_to(args.source.resolve())
        destination = seed / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        # Known canonical text inputs establish the unchanged LF reference.
        destination.write_bytes(path.read_bytes().replace(b'\r\n', b'\n'))
        relative_inputs.append(relative)

    controls = seed / 'tools' / 'local'
    controls.mkdir(parents=True)
    (controls / 'fixture.cmd').write_bytes(b'@echo off\necho control\n')
    (controls / 'fixture.bat').write_bytes(b'@echo off\necho control\n')
    hint_bytes = b'synthetic binary hint\r\n\x00\xff\n'
    (controls / 'fixture.hints').write_bytes(hint_bytes)
    run(args.git, '-C', str(seed), '-c', 'core.autocrlf=false', 'add', '--', '.')
    run(args.git, '-C', str(seed), '-c', 'user.name=Producer test',
        '-c', 'user.email=producer-test@example.invalid', 'commit', '--quiet', '-m', 'fixture')

    lf, crlf = fixture / 'lf', fixture / 'crlf'
    checkout(seed, lf, 'false', args.git)
    checkout(seed, crlf, 'true', args.git)
    golden, lf_inputs = configure(lf, fixture / 'lf-build', args.cmake)
    converted, crlf_inputs = configure(crlf, fixture / 'crlf-build', args.cmake)
    assert len(lf_inputs) == len(crlf_inputs) == len(selected), 'Checkout lost producer inputs'
    print(json.dumps({'current_working_tree': current, 'canonical_lf': golden,
                      'crlf_mode': converted, 'selected_inputs': len(selected)}), flush=True)
    assert golden == current, 'Current LF source identity changed in the canonical checkout'
    assert converted == golden, 'LF and CRLF-mode Git checkouts produce different cache identities'

    for relative in relative_inputs:
        expected = (seed / relative).read_bytes()
        assert (lf / relative).read_bytes() == expected
        assert (crlf / relative).read_bytes().replace(b'\r\n', b'\n') == expected, f'Producer input content changed: {relative}'
    assert run(args.git, '-C', str(crlf), 'status', '--porcelain', '--', 'src') == '', 'CRLF source checkout is not clean'

    for tree in [lf, crlf]:
        for suffix in ['cmd', 'bat']:
            command = (tree / 'tools' / 'local' / f'fixture.{suffix}').read_bytes()
            assert command == b'@echo off\r\necho control\r\n', 'Batch CRLF policy changed'
        assert (tree / 'tools' / 'local' / 'fixture.hints').read_bytes() == hint_bytes, 'Binary hints converted'

    (lf / 'unrelated.txt').write_text('Unrelated documentation does not invalidate inputs.\n')
    unrelated, _ = configure(lf, fixture / 'lf-build', args.cmake)
    assert unrelated == golden, 'Unrelated file edit invalidated the producer'
    changed = lf / relative_inputs[0]
    changed.write_bytes(changed.read_bytes() + b'\n// substantive producer fixture edit\n')
    invalidated, _ = configure(lf, fixture / 'lf-build', args.cmake)
    assert invalidated != golden, 'Actual producer input change did not invalidate identity'
    print('Static producer checkout identity and invalidation controls passed', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--scratch-root', type=Path, required=True)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--git', default='git')
    parser.add_argument('--keep-fixtures', action='store_true')
    args = parser.parse_args()
    args.source = args.source.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    scratch = args.scratch_root.resolve()
    fixture = Path(tempfile.mkdtemp(prefix='producer-', dir=scratch)).resolve()
    assert fixture.parent == scratch, 'Fixture escaped its declared scratch directory'
    try:
        check(args, fixture)
    finally:
        if args.keep_fixtures:
            print(f'Preserved fixture: {fixture}', flush=True)
        else:
            # Delete only this freshly created, verified child; never the source,
            # caller scratch directory, or an operator cache/save/config path.
            assert fixture.resolve().parent == scratch and fixture.name.startswith('producer-')
            def remove_readonly(function, path, error):
                if not isinstance(error, PermissionError):
                    raise error
                target = Path(path).resolve()
                target.relative_to(fixture)  # Reject any target outside this new fixture.
                mode = target.stat().st_mode
                if not target.is_file() or mode & stat.S_IWRITE:
                    raise error
                target.chmod(mode | stat.S_IWRITE)
                function(path)

            if sys.version_info >= (3, 12):
                shutil.rmtree(fixture, onexc=remove_readonly)
            else:
                shutil.rmtree(fixture, onerror=lambda function, path, error:
                              remove_readonly(function, path, error[1]))


if __name__ == '__main__':
    main()
