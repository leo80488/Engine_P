"""Run a bounded startup test against the executable built by CMake."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True)
    parser.add_argument('--root', required=True)
    parser.add_argument('--artifacts', required=True)
    parser.add_argument('--frames', type=int, default=90)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    artifacts = Path(args.artifacts).resolve() / str(time.time_ns())
    artifacts.mkdir(parents=True)
    command = [str(Path(args.exe).resolve()), '-unattended', '-hidden', '-scene=BaseScene',
               '-frames=' + str(args.frames), '-require_draws=20', '-maxerrors=0', '-maxseconds=180', '-accessguard_strict',
               '-settings=settings.default.json', '-artifacts=' + str(artifacts),
               '-user_dir=' + str(artifacts / 'user')]
    if Path(args.exe).stem == 'Game':
        command += ['-at_frame=' + str(max(1, args.frames - 10)), '-screenshot=shot.png', '-dumpstate=state.json']
    result = subprocess.run(command, cwd=root, timeout=210)
    ledger = artifacts / 'run.json'
    if not ledger.is_file():
        raise RuntimeError('No run.json: startup did not produce a verdict: ' + str(artifacts))
    data = json.loads(ledger.read_text(encoding='utf-8'))
    frames = artifacts / 'frames.jsonl'
    assert result.returncode == 0 and data['exitCode'] == 0 and data['verdict'] == 'pass', data
    assert data['framesRun'] >= args.frames, data['framesRun']
    assert data['counters']['engineErrors'] == 0 and data['counters']['accessGuardViolations'] == 0, data['counters']
    assert not data.get('unreadFlags'), data.get('unreadFlags')
    assert frames.is_file() and frames.stat().st_size > 0, 'No frame evidence'
    print(json.dumps({'exe': args.exe, 'verdict': data['verdict'], 'frames': data['framesRun'],
                      'counters': data['counters'], 'artifacts': str(artifacts)}, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
