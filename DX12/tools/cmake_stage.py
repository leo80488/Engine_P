"""Stage shared runtime files once for all native CMake applications."""
import argparse
import filecmp
from pathlib import Path
import shutil


def copy_changed(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.is_file() or not filecmp.cmp(source, destination, shallow=False):
        shutil.copy2(source, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--dll', action='append', default=[])
    args = parser.parse_args()
    root, out = Path(args.root), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for name in args.dll:
        source = Path(name)
        copy_changed(source, out / source.name)
    for source in (root / 'shaders').rglob('*'):
        if not source.is_file() or 'shader_cache_dxil' in source.parts:
            continue
        copy_changed(source, out / 'shaders' / source.relative_to(root / 'shaders'))
    copy_changed(root / 'engine.json', out / 'engine.json')
    print('CMake shared runtime staging: ready')


if __name__ == '__main__':
    main()
