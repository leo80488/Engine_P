"""Source ownership rules for the native CMake targets."""



import json

import os



RULES_REL = "tools/cmake_sources.json"

def load_rules(root):
    with open(os.path.join(root, RULES_REL), encoding="utf-8") as f:
        return json.load(f)

def glob_regex(pattern):
    import cmake_modules as gen_modules
    return gen_modules.glob_regex(pattern)

def walk(root, sub, exts):
    out = []
    for dp, _dn, fns in os.walk(os.path.join(root, sub)):
        for fn in fns:
            if fn.lower().endswith(exts):
                out.append(os.path.relpath(os.path.join(dp, fn), root).replace("\\", "/"))
    return sorted(out)

def module_owner(root):
    """The module-ownership table (tools/gen_modules.py): module of a DX12-relative path, or None."""
    import cmake_modules as gen_modules
    table = gen_modules.owner_table(gen_modules.load_descriptors(root))
    return lambda rel: gen_modules.module_of(rel, table)

def match_any(rel, patterns):
    return any(glob_regex(p).match(rel) for p in patterns)

def compile_sources(root, project, rules, owner):
    """The DX12-relative paths the project compiles (unordered)."""
    spec = rules["projects"][project]
    out = set(spec.get("pch", []))
    editor_core = set(rules["editorCoreSources"])
    engine_in_editor = set(rules["engineCoreInEditorDir"])
    per_exe = set(rules["perExe"])
    for rel in walk(root, "src", (".cpp",)):
        if rel in per_exe or rel in rules["notCompiledHere"] or rel == "src/pch.cpp":
            continue
        is_editor = (rel.startswith("src/Editor/") and rel not in engine_in_editor) or rel in editor_core
        if owner(rel) and not rel.startswith("src/Editor/"):
            continue                                   # an engine module's library (tools/gen_modules.py)
        if (project == "EditorCore") == is_editor:
            out.add(rel)
    out.update(spec.get("extra", []))
    return out
