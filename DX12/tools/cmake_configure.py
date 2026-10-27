"""Generate native CMake source lists and per-host module registration."""
import argparse
import json
from pathlib import Path
import cmake_modules as gen_modules
import cmake_source_rules as gen_sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True)
    parser.add_argument('--module', action='append', default=[])
    parser.add_argument('--no-harness', action='store_true')
    parser.add_argument('--no-control', action='store_true')
    parser.add_argument('--tracy', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    descs = gen_modules.load_descriptors(str(root))
    choices = {name: name in args.module for name in descs}
    enabled = gen_modules.resolve(descs, choices)
    owner = gen_sources.module_owner(str(root))
    rules = gen_sources.load_rules(str(root))
    core = gen_sources.compile_sources(str(root), 'EngineCore', rules, owner)
    editor = gen_sources.compile_sources(str(root), 'EditorCore', rules, owner)
    core.discard('src/pch.cpp')
    editor.discard('src/pch.cpp')
    editor = {p for p in editor if not owner(p) or owner(p) in enabled}
    if not args.tracy:
        core.discard('external/tracy/public/TracyClient.cpp')
    control = {'src/Harness/ControlChannel.cpp', 'src/Harness/ControlQuery.cpp',
               'src/Harness/ControlChannelJobs.cpp', 'src/Harness/CaptureRequests.cpp'}
    if args.no_harness:
        core = {p for p in core if not p.startswith('src/Harness/')}
    elif args.no_control:
        core -= control
    groups = {'ENGINE_CORE_SOURCES': core, 'ENGINE_EDITOR_SOURCES': editor}
    for name in enabled:
        if descs[name]['kind'] != 'cpp':
            continue
        groups['ENGINE_MODULE_' + name + '_SOURCES'] = {
            p.relative_to(root).as_posix() for p in (root / 'src').rglob('*.cpp')
            if owner(p.relative_to(root).as_posix()) == name
            and not p.relative_to(root).as_posix().startswith('src/Editor/')}
    text = []
    for name, paths in groups.items():
        text.append('set(' + name)
        text.extend('    "${ENGINE_ROOT}/' + p + '"' for p in sorted(paths))
        text.append(')')
    text.append('set(ENGINE_CPP_MODULES ' + ' '.join(n for n in enabled if descs[n]['kind'] == 'cpp') + ')')
    (out / 'Sources.cmake').write_text('\n'.join(text) + '\n', encoding='utf-8')
    gen_modules.write_set(str(out / 'modules'), descs, enabled, 'CMake', str(root))
    print('Native CMake: %d core, %d editor source files; modules: %s' %
          (len(core), len(editor), ', '.join(enabled) or 'none'))


if __name__ == '__main__':
    main()
