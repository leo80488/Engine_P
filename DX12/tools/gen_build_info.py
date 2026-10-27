#!/usr/bin/env python3
"""The build's identity, generated before each compile (props/Generators.targets target GenBuildInfo): what a log, a
crash report, a run.json, the exe's VERSIONINFO and a package report name a build by.

    python tools/gen_build_info.py --out <dir> --config Retail [--project <dir with game.json>]

Writes, each only when its content changes (so an unchanged identity recompiles nothing):
    <dir>/System/BuildInfoValues.h   the values src/System/BuildInfo.cpp compiles in
    <dir>/Platform/BuildInfo.rc2     the VERSIONINFO block src/Platform/App.rc includes

The identity: game.json's "name" and "version" (version "major.minor.patch"), the git commit (12 hex),
whether the working tree was dirty, the branch, the configuration, the VARIANT (the harness on or off, CpuArch,
whole-program optimisation, the module set), the toolchain (the MSVC and Windows SDK versions) and the deps id
(external/deps.json: the pinned third-party payload), and buildId - 16 hex of FNV-1a over all of them. A QA
package (Retail with the harness), the shipped one and an AVX2 A/B are three builds of one commit and get three
ids; so do two compilers. No date: two builds of one tree in one configuration are the same build (and /Brepro
binaries compare equal). Checked-in fallbacks with the same include names (include/System,
include/Platform) stand in when this has not run (no Python: the build still succeeds, as "unknown").

The variant and toolchain come from the build (props/Generators.targets passes them); without them the variant is the
configuration's own (harness on except in Retail, whole-program optimisation only in Retail). The VERSIONINFO
Comments line carries all of it and ends "build <id>)", which tools/package_audit.py build_identity reads.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import uuid

DX12 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args):
    try:
        r = subprocess.run(["git"] + list(args), cwd=DX12, capture_output=True, text=True, timeout=30)
        return r.stdout.strip() if r.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def fnv64(text):
    h = 14695981039346656037
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def truth(text, default):
    """An MSBuild boolean ("true", "1", "false", "0"); @p default for an empty or unknown one."""
    t = (text or "").strip().lower()
    if t in ("true", "1", "yes", "on"):
        return True
    if t in ("false", "0", "no", "off"):
        return False
    return default


def module_set_name(module_set):
    """'default' (the checked-in module list), or the enabled modules of a -p:ModuleSet build ('core' when none):
    the set's content, not the folder it was generated into."""
    module_set = (module_set or "").strip().strip('"').rstrip("\\/")
    if not module_set:
        return "default"
    try:
        with open(os.path.join(module_set, "modules.json"), encoding="utf-8") as f:
            enabled = json.load(f).get("enabled", [])
        return "+".join(sorted(enabled)) or "core"
    except (OSError, ValueError):
        return os.path.basename(module_set) or "set"


def variant_of(config, harness="", cpu_arch="", wpo="", module_set=""):
    """The build flags that change the binary without changing the commit or the configuration name."""
    return {"harness": truth(harness, config != "Retail"),
            "cpuArch": (cpu_arch or "").strip().lower() or "default",
            "wpo": truth(wpo, config == "Retail"),
            "modules": module_set_name(module_set)}


def variant_text(v):
    return "harness %s, wpo %s, arch %s, modules %s" % ("on" if v["harness"] else "off", "on" if v["wpo"] else "off",
                                                        v["cpuArch"], v["modules"])


def tool_versions(tools_dir):
    """{'cl', 'link'}: the file versions of the compiler and linker in the toolset folder @p tools_dir
    ($(VCToolsInstallDir)); empty when it is unknown or the tools are not there. They are the build's actual
    tools: an in-place update changes them and keeps the folder (and so $(VCToolsVersion))."""
    tools_dir = (tools_dir or "").strip().strip('"').rstrip("\\/")
    if not tools_dir:
        return {}
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import build as build_tool
        return build_tool.msvc_tool_versions(tools_dir)
    except Exception:           # the identity must never stop a build; a missing version is just not recorded
        return {}


def toolchain_text(msvc="", sdk="", cl="", link=""):
    """'msvc <toolset folder> cl <compiler file version> link <linker file version> sdk <Windows SDK>': what a
    binary was built with, down to the compiler's own build (the compiler and linker parts only when known)."""
    parts = []
    if msvc and msvc.strip():
        parts.append("msvc " + msvc.strip())
    if cl and cl.strip():
        parts.append("cl " + cl.strip())
    if link and link.strip():
        parts.append("link " + link.strip())
    if sdk and sdk.strip():
        parts.append("sdk " + sdk.strip().rstrip("\\/"))
    return " ".join(parts) or "unknown"


def deps_id():
    """12 hex of external/deps.json's pinned content (tools/fetch_deps.py --digest), '' without a manifest."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import fetch_deps
        return fetch_deps.short_digest()
    except Exception:       # a broken manifest must not stop the build; the verify step says so
        return ""


def identity(project_dir, config, variant=None, toolchain="unknown", deps=None):
    name, version = "Game", "0.0.0"
    try:
        with open(os.path.join(project_dir, "game.json"), encoding="utf-8") as f:
            manifest = json.load(f)
        name = str(manifest.get("name") or name)
        version = str(manifest.get("version") or version)
    except (OSError, ValueError):
        pass
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        print("gen_build_info: game.json version '%s' is not major.minor.patch; using 0.0.0" % version)
        version = "0.0.0"
    sha = git("rev-parse", "--short=12", "HEAD") or "unknown"
    # Untracked files do not make a build dirty: a run's artifacts are untracked.
    dirty = bool(git("status", "--porcelain", "--untracked-files=no")) if sha != "unknown" else False
    branch = git("rev-parse", "--abbrev-ref", "HEAD") or "unknown"
    variant = variant or variant_of(config)
    deps = deps_id() if deps is None else deps
    key = "|".join([name, version, sha, "dirty" if dirty else "clean", branch, config, variant_text(variant),
                    toolchain, deps])
    return {"product": name, "version": version, "gitSha": sha, "dirty": dirty, "branch": branch, "config": config,
            "variant": variant, "toolchain": toolchain, "deps": deps, "buildId": "%016x" % fnv64(key)}


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def header(i):
    major, minor, patch = (int(x) for x in i["version"].split("."))
    return ("// Generated by tools/gen_build_info.py - do not edit.\n"
            "#pragma once\n"
            "#define BUILDINFO_PRODUCT  %s\n"
            "#define BUILDINFO_VERSION  %s\n"
            "#define BUILDINFO_MAJOR    %d\n"
            "#define BUILDINFO_MINOR    %d\n"
            "#define BUILDINFO_PATCH    %d\n"
            "#define BUILDINFO_GIT_SHA  %s\n"
            "#define BUILDINFO_DIRTY    %d\n"
            "#define BUILDINFO_BRANCH   %s\n"
            "#define BUILDINFO_CONFIG   %s\n"
            "#define BUILDINFO_BUILD_ID %s\n"
            % (c_string(i["product"]), c_string(i["version"]), major, minor, patch, c_string(i["gitSha"]),
               1 if i["dirty"] else 0, c_string(i["branch"]), c_string(i["config"]), c_string(i["buildId"])))


def rc2(i):
    major, minor, patch = (int(x) for x in i["version"].split("."))
    detail = "%s %s (%s%s, %s, %s, %s, deps %s, build %s)" % (
        i["product"], i["version"], i["gitSha"], "+dirty" if i["dirty"] else "", i["config"], variant_text(i["variant"]),
        i["toolchain"], i["deps"] or "none", i["buildId"])
    return ("// Generated by tools/gen_build_info.py - do not edit.\n"
            "VS_VERSION_INFO VERSIONINFO\n"
            " FILEVERSION %d,%d,%d,0\n"
            " PRODUCTVERSION %d,%d,%d,0\n"
            " FILEFLAGSMASK 0x3fL\n"
            " FILEFLAGS 0x0L\n"
            " FILEOS 0x40004L\n"
            " FILETYPE 0x1L\n"
            " FILESUBTYPE 0x0L\n"
            "BEGIN\n"
            "    BLOCK \"StringFileInfo\"\n"
            "    BEGIN\n"
            "        BLOCK \"040904b0\"\n"
            "        BEGIN\n"
            "            VALUE \"ProductName\", %s\n"
            "            VALUE \"ProductVersion\", %s\n"
            "            VALUE \"FileVersion\", %s\n"
            "            VALUE \"FileDescription\", %s\n"
            "            VALUE \"Comments\", %s\n"
            "        END\n"
            "    END\n"
            "    BLOCK \"VarFileInfo\"\n"
            "    BEGIN\n"
            "        VALUE \"Translation\", 0x409, 1200\n"
            "    END\n"
            "END\n"
            % (major, minor, patch, major, minor, patch, c_string(i["product"]), c_string(i["version"]),
               c_string(i["version"]), c_string(i["product"]), c_string(detail)))


def write_if_changed(path, text):
    """Write @p text unless the file already holds it. Atomic: eight projects run this on one folder at the
    same time, and a compiler reading a half-written header is a build that fails for no reason (or compiles a
    torn identity) - the new content is written beside the file and renamed over it."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    tmp = "%s.%d.%s.tmp" % (path, os.getpid(), uuid.uuid4().hex[:8])
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        for _ in range(40):                 # a reader holding the target open makes the rename fail on Windows
            try:
                os.replace(tmp, path)
                return True
            except PermissionError:
                time.sleep(0.05)
        os.replace(tmp, path)
        return True
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="the generated folder (props/Engine.Cpp.props: <BuildRoot><Platform>/<Config>/generated)")
    ap.add_argument("--config", required=True)
    ap.add_argument("--project", default=DX12, help="the folder holding game.json")
    ap.add_argument("--harness", default="", help="$(EnableHarness): true / false (default: on except in Retail)")
    ap.add_argument("--cpu-arch", default="", help="$(CpuArch): AVX2, or empty")
    ap.add_argument("--wpo", default="", help="$(WholeProgramOptimization): true / false (default: on in Retail)")
    ap.add_argument("--module-set", default="", help="$(ModuleSet): the folder of a gen_modules.py set, or empty")
    ap.add_argument("--msvc", default="", help="$(VCToolsVersion)")
    ap.add_argument("--sdk", default="", help="$(TargetPlatformVersion)")
    ap.add_argument("--tools", default="", help="$(VCToolsInstallDir): the toolset folder, to read cl's and link's versions")
    a = ap.parse_args()
    tools = tool_versions(a.tools)
    i = identity(a.project, a.config, variant_of(a.config, a.harness, a.cpu_arch, a.wpo, a.module_set),
                 toolchain_text(a.msvc, a.sdk, tools.get("cl", ""), tools.get("link", "")))
    changed = [p for p, text in ((os.path.join(a.out, "System", "BuildInfoValues.h"), header(i)),
                                 (os.path.join(a.out, "Platform", "BuildInfo.rc2"), rc2(i)))
               if write_if_changed(p, text)]
    print("gen_build_info: %s %s (%s%s, %s, %s, %s, deps %s, build %s)%s" % (
        i["product"], i["version"], i["gitSha"], "+dirty" if i["dirty"] else "", i["config"], variant_text(i["variant"]),
        i["toolchain"], i["deps"] or "none", i["buildId"], " - updated" if changed else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
