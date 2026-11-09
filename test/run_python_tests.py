#!/usr/bin/env python3
"""Run the standard-library Python regression suite."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
TEST = ROOT / 'test'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitizers', action='store_true',
                        help='also compile and run ASan/UBSan smoke tests')
    parser.add_argument('--binary', default='./clawc', help='binary for existing Python CLI and ELF checks')
    parser.add_argument('--skip-harden', action='store_true', help='skip ELF hardening check for non-ELF or HARDEN=0')
    options = parser.parse_args()
    os.chdir(ROOT)
    binary = str(Path(options.binary).resolve())
    if not Path(binary).is_file():
        print('Missing binary; run make first: ' + binary, file=sys.stderr)
        return 1
    os.environ['CLAWC_TEST_BIN'] = binary
    sys.path.insert(0, str(TEST))
    names = ['test_core_python', 'test_integration_python', 'test_swarm_python']
    if options.sanitizers:
        names.append('test_asan_python')
    suite = unittest.TestLoader().loadTestsFromNames(names)
    print('== Python regression tests ==', flush=True)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    ok = result.wasSuccessful()
    # These scripts are already Python and remain separate until their cases
    # are refactored into unittest. Never suppress their failures.
    scripts = [(['test/ministrip_test.py'], 'ELF minimizer'),
               (['test/cli_config_test.py', binary], 'CLI configuration'),
               (['test/swarm_index_test.py', binary], 'Swarm index')]
    if not options.skip_harden and sys.platform.startswith('linux'):
        scripts.insert(1, (['test/elf_hardening_test.py', binary], 'ELF hardening'))
    # Bridge script has additional fsync-failure checks only if CLAWC_TEST_BIN
    # is absent. Invoke it without that variable when using the default build.
    scripts.append((['test/bridge_workers_test.py'], 'Bridge workers'))
    for args, title in scripts:
        env = os.environ.copy()
        if args[0] == 'test/bridge_workers_test.py':
            if Path(binary) == (ROOT / 'clawc'):
                env.pop('CLAWC_TEST_BIN', None)
            else:
                print('Note: bridge fsync failure injection is skipped with a custom binary.', flush=True)
        print('== ' + title + ' ==', flush=True)
        completed = subprocess.run([sys.executable, *args], cwd=ROOT, env=env)
        ok = ok and completed.returncode == 0
    return 0 if ok else 1

if __name__ == '__main__':
    raise SystemExit(main())
