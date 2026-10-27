#!/usr/bin/env python3
"""fetch_deps.py - the untracked third-party payload the build and the shipped exe need, pinned by hash.

    python tools/fetch_deps.py --verify              # every file of external/deps.json present with its sha256
    python tools/fetch_deps.py --verify --quick      # presence and byte size only (the doctor's check)
    python tools/fetch_deps.py --from <checkout>     # copy what is missing from a checkout that has it
    python tools/fetch_deps.py                       # fetch what is missing from each dep's "source" url
    python tools/fetch_deps.py --digest              # the manifest's identity (what a build id and a ship report carry)
    python tools/fetch_deps.py --write-manifest --from <checkout>   # maintainers: hash that payload into deps.json

Git holds the headers of NRD, meshoptimizer, Recast / Detour, assimp, DXC and Streamline and not their
libraries, DLLs and prebuilt trees (ENGINE_REVIEW_2026-10-25 F1 / N1). external/deps.json lists every file of
them a build or a shipped exe reads, with its version, size and sha256, so a fresh clone can tell what it lacks,
a build identity can name the payload it was linked with, and a ship report can say whether the payload that
shipped is the pinned one. RTXDI and SHARC are vendored from tracked archives (vendor_rtxdi.py, vendor_sharc.py)
and ONNX Runtime from a pinned download (vendor_onnxruntime.py); they are not listed here.

--from takes the DX12/ folder of a checkout that has the payload (or its external/ folder). A file is copied only
when the source file's sha256 equals the manifest's; a destination under a junction or symlink is left alone (a
worktree's junction into the main checkout is that checkout's own copy, and writing through it would change it).
"Where the payload is downloaded from" is decision D-3 of the review and is open: a dep's source.url is empty
until it is made; with one, the archive is downloaded to build/deps/, checked against source.sha256 and its
files extracted under source.stripPrefix.

Exit: 0 every required file present and exact; 1 a required file missing or any present file different;
2 usage / nothing to fetch from.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import shutil
import stat
import sys
import urllib.request
import zipfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
DX12_DIR = os.path.dirname(TOOLS_DIR)
EXTERNAL = os.path.join(DX12_DIR, "external")
MANIFEST = os.path.join(EXTERNAL, "deps.json")
CACHE = os.path.join(DX12_DIR, "build", "deps")
SCHEMA = 1


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load_manifest(path=MANIFEST):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    if data.get("schema") != SCHEMA:
        raise ValueError("%s: schema %r, this tool reads %d" % (path, data.get("schema"), SCHEMA))
    return data


def manifest_digest(manifest):
    """sha256 over the pinned content only (names, versions, file hashes): a comment, a note or a source url
    changing does not change the identity a build carries. 12 hex digits of it are the 'deps' of a build id."""
    pinned = {d["name"]: {"version": d.get("version", ""), "required": bool(d.get("required", True)),
                          "files": {p: f["sha256"] for p, f in sorted(d.get("files", {}).items())},
                          "optionalFiles": {p: f["sha256"] for p, f in sorted(d.get("optionalFiles", {}).items())}}
              for d in manifest["deps"]}
    text = json.dumps(pinned, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def short_digest(manifest_path=MANIFEST):
    """The 12-hex deps id for a build identity, or '' without a manifest."""
    try:
        return manifest_digest(load_manifest(manifest_path))[:12]
    except (OSError, ValueError, KeyError):
        return ""


def is_linked(path, stop):
    """A junction or symlink at @p path or any parent up to @p stop (the external folder)."""
    p = os.path.abspath(path)
    stop = os.path.abspath(stop)
    while True:
        try:
            st = os.lstat(p)
        except OSError:
            st = None
        # a junction is a reparse point (os.path.isjunction needs Python 3.12; the engine needs 3.10)
        if st is not None and (stat.S_ISLNK(st.st_mode)
                               or getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)):
            return p
        if p == stop or os.path.dirname(p) == p:
            return None
        p = os.path.dirname(p)


def expand(base, patterns):
    """Files under @p base matching any of the glob @p patterns ('**' crosses folders), relative, sorted."""
    found = set()
    for pat in patterns:
        pat = pat.replace("\\", "/")
        for dp, _dn, fns in os.walk(base):
            for fn in fns:
                rel = os.path.relpath(os.path.join(dp, fn), base).replace("\\", "/")
                if _glob_match(rel, pat):
                    found.add(rel)
    return sorted(found)


def _glob_match(rel, pat):
    parts = pat.split("/")
    segs = rel.split("/")

    def rec(i, j):
        if i == len(parts):
            return j == len(segs)
        if parts[i] == "**":
            return any(rec(i + 1, k) for k in range(j, len(segs) + 1))
        return j < len(segs) and fnmatch.fnmatchcase(segs[j], parts[i]) and rec(i + 1, j + 1)
    return rec(0, 0)


def external_dir(path):
    """@p path is a DX12/ folder or an external/ folder."""
    path = os.path.abspath(path)
    inner = os.path.join(path, "external")
    return inner if os.path.isdir(inner) and not os.path.isfile(os.path.join(path, "deps.json")) else path


# ------------------------------------------------------------------ manifest ----
def write_manifest(payload_external, manifest_path=MANIFEST):
    """Hash the payload at @p payload_external into the manifest's file lists (each dep's include patterns,
    optionalInclude patterns). Everything else in the manifest is kept."""
    manifest = load_manifest(manifest_path)
    for dep in manifest["deps"]:
        base = os.path.join(payload_external, dep["root"])
        dep["files"], dep["optionalFiles"] = {}, {}
        for key, patterns in (("files", dep.get("include", [])), ("optionalFiles", dep.get("optionalInclude", []))):
            for rel in expand(base, patterns):
                path = os.path.join(base, rel)
                dep[key][rel] = {"sha256": sha256_file(path), "bytes": os.path.getsize(path)}
        if dep.get("required", True) and not dep["files"]:
            raise ValueError("%s: no file of %s matches its include patterns under %s" % (dep["name"], dep["include"], base))
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
        f.write("\n")
    return manifest


# -------------------------------------------------------------------- verify ----
def verify(external=EXTERNAL, manifest=None, quick=False, only=None):
    """-> {'ok', 'digest', 'deps': [{'name','required','status','missing':[],'mismatched':[],'files','bytes'}]}.
    status: ok | missing (a required dep with a file absent) | mismatch (any file differs) | absent (an optional
    dep with no file here: fine) | partial (an optional dep with some files: a broken install)."""
    manifest = manifest or load_manifest()
    deps, ok = [], True
    for dep in manifest["deps"]:
        if only and dep["name"] != only:
            continue
        required = bool(dep.get("required", True))
        base = os.path.join(external, dep["root"])
        missing, mismatched, present, total_bytes = [], [], 0, 0
        for group, need in (("files", True), ("optionalFiles", False)):
            for rel, want in dep.get(group, {}).items():
                path = os.path.join(base, rel)
                if not os.path.isfile(path):
                    if need:
                        missing.append(rel)
                    continue
                present += 1
                size = os.path.getsize(path)
                total_bytes += size
                if size != want["bytes"] or (not quick and sha256_file(path) != want["sha256"]):
                    mismatched.append(rel)
        if mismatched:
            status = "mismatch"
        elif not missing:
            status = "ok"
        elif required:
            status = "missing"
        elif present == 0:
            status = "absent"
        else:
            status = "partial"
        if status in ("mismatch", "missing", "partial"):
            ok = False
        deps.append({"name": dep["name"], "version": dep.get("version", ""), "required": required, "status": status,
                     "missing": missing, "mismatched": mismatched, "files": present, "bytes": total_bytes})
    return {"ok": ok, "quick": quick, "digest": manifest_digest(manifest)[:12], "deps": deps}


def print_report(report, limit=6):
    for d in report["deps"]:
        extra = ""
        if d["missing"]:
            extra += "  missing %d (%s%s)" % (len(d["missing"]), ", ".join(d["missing"][:limit]),
                                               ", ..." if len(d["missing"]) > limit else "")
        if d["mismatched"]:
            extra += "  DIFFERENT %d (%s%s)" % (len(d["mismatched"]), ", ".join(d["mismatched"][:limit]),
                                                ", ..." if len(d["mismatched"]) > limit else "")
        print("[deps] %-16s %-9s %-8s %3d file(s) %7.1f MB%s" % (
            d["name"], d["status"], "required" if d["required"] else "optional", d["files"], d["bytes"] / 2**20, extra))
    print("[deps] %s - manifest %s%s" % ("OK" if report["ok"] else "FAIL", report["digest"],
                                         " (sizes only: --quick)" if report["quick"] else ""))


# --------------------------------------------------------------------- fetch ----
def _copy_verified(src, dst, want, notes):
    if os.path.isfile(src) and sha256_file(src) == want["sha256"]:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + ".part"
        shutil.copyfile(src, tmp)
        os.replace(tmp, dst)
        return True
    notes.append("%s: %s" % ("the source file differs from the manifest" if os.path.isfile(src) else "not in the source",
                             src))
    return False


def fetch(external=EXTERNAL, manifest=None, source=None, only=None):
    """Bring missing / different files in line with the manifest, from @p source (an external/ folder of a checkout
    that has them) or each dep's source.url. -> {'copied': n, 'skipped': [...], 'notes': [...], 'unsourced': [names]}."""
    manifest = manifest or load_manifest()
    out = {"copied": 0, "skipped": [], "notes": [], "unsourced": []}
    for dep in manifest["deps"]:
        if only and dep["name"] != only:
            continue
        base = os.path.join(external, dep["root"])
        todo = []
        for group in ("files", "optionalFiles"):
            for rel, want in dep.get(group, {}).items():
                path = os.path.join(base, rel)
                if os.path.isfile(path) and sha256_file(path) == want["sha256"]:
                    continue
                todo.append((rel, want, group == "files"))
        if not todo:
            continue
        link = is_linked(base, external)
        if link:
            out["skipped"].append("%s: %s is a junction or symlink; left alone" % (dep["name"], link))
            continue
        archive = None
        if source is None:
            url = (dep.get("source") or {}).get("url")
            if not url:
                out["unsourced"].append(dep["name"])
                continue
            archive = _download(dep, url, out["notes"])
            if archive is None:
                continue
        for rel, want, needed in todo:
            dst = os.path.join(base, rel)
            if archive is not None:
                ok = _extract_one(archive, (dep.get("source") or {}).get("stripPrefix", ""), rel, dst, want, out["notes"])
            else:
                ok = _copy_verified(os.path.join(source, dep["root"], rel), dst, want, out["notes"])
            if ok:
                out["copied"] += 1
            elif needed:
                out["notes"].append("%s: %s not fetched" % (dep["name"], rel))
    return out


def _download(dep, url, notes):
    os.makedirs(CACHE, exist_ok=True)
    name = "%s-%s.zip" % (dep["name"], dep.get("version") or "x")
    path = os.path.join(CACHE, name)
    want = (dep.get("source") or {}).get("sha256")
    if not (os.path.isfile(path) and (not want or sha256_file(path) == want)):
        tmp = path + ".part"
        try:
            with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
        except (OSError, ValueError) as e:
            notes.append("%s: download of %s failed: %s" % (dep["name"], url, e))
            return None
        os.replace(tmp, path)
    if want and sha256_file(path) != want:
        notes.append("%s: %s does not match source.sha256" % (dep["name"], name))
        return None
    return path


def _extract_one(archive, prefix, rel, dst, want, notes):
    member = (prefix + rel).replace("\\", "/")
    try:
        with zipfile.ZipFile(archive) as z:
            data = z.read(member)
    except (KeyError, zipfile.BadZipFile, OSError):
        notes.append("%s is not in %s" % (member, os.path.basename(archive)))
        return False
    if hashlib.sha256(data).hexdigest() != want["sha256"]:
        notes.append("%s in %s differs from the manifest" % (member, os.path.basename(archive)))
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dst)
    return True


# ----------------------------------------------------------------------- cli ----
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true", help="check the payload against the manifest (exit 1 on a gap)")
    ap.add_argument("--quick", action="store_true", help="with --verify: presence and byte size, no hashing")
    ap.add_argument("--from", dest="source", metavar="DIR", help="a checkout's DX12/ (or external/) folder to copy from")
    ap.add_argument("--write-manifest", action="store_true",
                    help="hash the payload (this tree's, or --from's) into deps.json's file lists")
    ap.add_argument("--digest", action="store_true", help="print the manifest identity (12 hex) and exit")
    ap.add_argument("--only", metavar="NAME", help="one dep")
    ap.add_argument("--json", action="store_true", help="with --verify: the report as JSON")
    ap.add_argument("--root", default=EXTERNAL, help="the external/ folder to read and fill (default this checkout's)")
    ap.add_argument("--manifest", default=MANIFEST)
    a = ap.parse_args(argv)
    if a.digest:
        print(manifest_digest(load_manifest(a.manifest))[:12])
        return 0
    if a.write_manifest:
        payload = external_dir(a.source) if a.source else a.root
        manifest = write_manifest(payload, a.manifest)
        print("[deps] %s: %d dep(s), %d file(s) hashed from %s" % (
            a.manifest, len(manifest["deps"]), sum(len(d["files"]) + len(d["optionalFiles"]) for d in manifest["deps"]),
            payload))
        return 0
    manifest = load_manifest(a.manifest)
    fetching = not a.verify or bool(a.source)
    unsourced = []
    if fetching:
        result = fetch(a.root, manifest, external_dir(a.source) if a.source else None, a.only)
        unsourced = [] if a.source else result["unsourced"]
        for line in result["skipped"] + result["notes"]:
            print("[deps] " + line)
        if unsourced:
            print("[deps] no source for %s: its source.url is empty (review decision D-3 is open). Copy from a checkout "
                  "that has the payload: python tools/fetch_deps.py --from <that checkout's DX12 folder>"
                  % ", ".join(unsourced))
        print("[deps] copied %d file(s)" % result["copied"])
    report = verify(a.root, manifest, quick=a.quick, only=a.only)
    if a.json:
        print(json.dumps(report, indent=1))
    else:
        print_report(report)
    if report["ok"]:
        return 0
    return 2 if unsourced else 1


if __name__ == "__main__":
    sys.exit(main())
