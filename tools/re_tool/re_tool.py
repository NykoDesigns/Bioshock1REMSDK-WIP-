#!/usr/bin/env python3
"""
re_tool.py - Reverse-engineering CLI for the BioShock1 SDK.

A single entry point that hooks into Ghidra (headless), umodel (UE Viewer),
and UE Explorer so analysis queries can be run from the command line and the
results consumed as JSON / plain text.

Examples
--------
  # Ghidra: decompile a function by name or address
  python re_tool.py decompile FBspSurf
  python re_tool.py decompile 0x14012ab30

  # Ghidra: list functions whose name contains a substring
  python re_tool.py func LightMap

  # Ghidra: dump a struct/datatype layout
  python re_tool.py struct FLightMapIndex

  # Ghidra: references to a symbol
  python re_tool.py xref UModel::Serialize

  # Ghidra: find an ASCII string in memory and its xrefs
  python re_tool.py search LightMap

  # Ghidra: list defined strings containing a substring
  python re_tool.py strings lightmap

  # Ghidra: hex dump bytes at an address
  python re_tool.py data 0x14012ab30 64

  # umodel: list a package's objects
  python re_tool.py umodel-list 1-Medical.bsm

  # umodel: export a package (or one object) to the export dir
  python re_tool.py umodel-export 1-Medical.bsm --type Texture

Notes
-----
* Ghidra commands require the project to NOT be open in the Ghidra GUI
  (headless takes an exclusive lock). Close the GUI first.
* Ghidra analysis is skipped (-noanalysis); the program is already analyzed.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "config.json")
SCRIPT_DIR = os.path.join(HERE, "ghidra_scripts")


def load_config():
    with open(CONFIG_PATH, "r") as f:
        return json.load(f)


CFG = load_config()


# ---------------------------------------------------------------------------
# Ghidra headless
# ---------------------------------------------------------------------------

def run_ghidra_query(command, args):
    """Run GhidraQuery.py headless with the given command + positional args.

    Returns the parsed JSON result, or a dict with an 'error' key.
    """
    analyze = os.path.join(CFG["ghidra_install"], "support", "analyzeHeadless.bat")
    if not os.path.exists(analyze):
        return {"error": "analyzeHeadless.bat not found at {}".format(analyze)}

    out_fd, out_path = tempfile.mkstemp(suffix=".json", prefix="ghquery_")
    os.close(out_fd)

    script_args = [command] + list(args) + [out_path]
    cmd = [
        analyze,
        CFG["ghidra_project_dir"],
        CFG["ghidra_project_name"],
        "-process", CFG["ghidra_program"],
        "-noanalysis",
        "-scriptPath", SCRIPT_DIR,
        "-postScript", "GhidraQuery.java",
    ] + script_args

    print("[re_tool] running Ghidra headless: {} {}".format(command, " ".join(args)))
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        return {"error": "Ghidra headless timed out (900s)"}

    result = None
    if os.path.exists(out_path):
        try:
            with open(out_path, "r") as f:
                content = f.read().strip()
            if content:
                result = json.loads(content)
        except Exception as e:
            result = {"error": "failed to parse Ghidra output: {}".format(e)}
        finally:
            try:
                os.remove(out_path)
            except OSError:
                pass

    if result is None:
        tail = (proc.stderr or proc.stdout or "")[-1500:]
        result = {"error": "no output produced by Ghidra", "log_tail": tail}
    return result


def cmd_ghidra(command, args):
    result = run_ghidra_query(command, args)
    # Decompiled code is nicer printed raw than JSON-escaped
    if command == "decompile" and "code" in result:
        print("// {}  @ {}".format(result.get("name"), result.get("entry")))
        print("// {}".format(result.get("signature")))
        print(result["code"])
    else:
        print(json.dumps(result, indent=2))
    return 0 if "error" not in result else 1


# ---------------------------------------------------------------------------
# umodel (UE Viewer)
# ---------------------------------------------------------------------------

def run_umodel(extra_args):
    umodel = CFG["umodel_exe"]
    if not os.path.exists(umodel):
        print("[re_tool] umodel not found: {}".format(umodel))
        return 1
    cmd = [umodel] + extra_args
    print("[re_tool] umodel {}".format(" ".join(extra_args)))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.stdout:
        print(proc.stdout)
    if proc.returncode != 0 and proc.stderr:
        print(proc.stderr, file=sys.stderr)
    return proc.returncode


def resolve_package(pkg):
    """Allow passing just a package name; resolve against the content dir."""
    if os.path.isabs(pkg) and os.path.exists(pkg):
        return pkg
    candidate = os.path.join(CFG["game_content_dir"], "Maps", pkg)
    if os.path.exists(candidate):
        return candidate
    # search the content tree
    for root, _dirs, files in os.walk(CFG["game_content_dir"]):
        if pkg in files:
            return os.path.join(root, pkg)
    return pkg  # let umodel try


def cmd_umodel_list(pkg):
    path = resolve_package(pkg)
    return run_umodel([
        "-path={}".format(CFG["game_content_dir"]),
        "-game={}".format(CFG["umodel_game_tag"]),
        "-list",
        path,
    ])


def cmd_umodel_export(pkg, obj_type, obj_name):
    path = resolve_package(pkg)
    args = [
        "-path={}".format(CFG["game_content_dir"]),
        "-game={}".format(CFG["umodel_game_tag"]),
        "-export",
        "-out={}".format(CFG["umodel_export_dir"]),
        path,
    ]
    if obj_name:
        args.append(obj_name)
    if obj_type:
        args.append(obj_type)
    return run_umodel(args)


# ---------------------------------------------------------------------------
# UnrealScript decompiler (headless replacement for UE Explorer's GUI)
# ---------------------------------------------------------------------------

def cmd_uscript_export():
    """Decompile the .U script packages to .uc source via the UELib CLI tool."""
    exe = CFG["uscript_decompiler_exe"]
    if not os.path.exists(exe):
        print("[re_tool] decompiler not built: {}\n"
              "Build it with: dotnet build tools\\decompiler -c Release".format(exe))
        return 1
    cmd = [exe, CFG["baked_scripts_dir"], CFG["decompiled_dir"]]
    print("[re_tool] uscript-export -> {}".format(CFG["decompiled_dir"]))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.stdout:
        print(proc.stdout)
    if proc.returncode != 0 and proc.stderr:
        print(proc.stderr, file=sys.stderr)
    return proc.returncode


def cmd_uscript_grep(pattern, ext_filter):
    """Search the decompiled .uc source tree for a pattern (regex, case-insensitive)."""
    import re
    root = CFG["decompiled_dir"]
    if not os.path.isdir(root):
        print("[re_tool] no decompiled output at {}\n"
              "Run: python re_tool.py uscript-export".format(root))
        return 1
    rx = re.compile(pattern, re.IGNORECASE)
    hits = 0
    for dpath, _dirs, files in os.walk(root):
        for fn in files:
            if not fn.lower().endswith(".uc"):
                continue
            fp = os.path.join(dpath, fn)
            try:
                with open(fp, "r", encoding="utf-8", errors="replace") as f:
                    for i, line in enumerate(f, 1):
                        if rx.search(line):
                            rel = os.path.relpath(fp, root)
                            print("{}:{}: {}".format(rel, i, line.rstrip()))
                            hits += 1
                            if hits >= 400:
                                print("... (truncated at 400 matches)")
                                return 0
            except OSError:
                continue
    print("[re_tool] {} matches".format(hits))
    return 0


# ---------------------------------------------------------------------------
# arg parsing
# ---------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(description="BioShock1 SDK reverse-engineering CLI")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("decompile", help="Ghidra: decompile a function")
    sp.add_argument("target", help="function name or address (0x...)")

    sp = sub.add_parser("func", help="Ghidra: list functions matching a substring")
    sp.add_argument("pattern")

    sp = sub.add_parser("struct", help="Ghidra: dump a struct/datatype layout")
    sp.add_argument("name")

    sp = sub.add_parser("xref", help="Ghidra: references to a symbol")
    sp.add_argument("name")

    sp = sub.add_parser("search", help="Ghidra: find an ASCII string in memory + xrefs")
    sp.add_argument("text")

    sp = sub.add_parser("strings", help="Ghidra: list defined strings containing substring")
    sp.add_argument("substr")

    sp = sub.add_parser("data", help="Ghidra: hex dump bytes at an address")
    sp.add_argument("addr")
    sp.add_argument("count", type=int)

    sp = sub.add_parser("umodel-list", help="umodel: list objects in a package")
    sp.add_argument("package")

    sp = sub.add_parser("umodel-export", help="umodel: export a package or object")
    sp.add_argument("package")
    sp.add_argument("--type", default="", help="object class (e.g. Texture, StaticMesh)")
    sp.add_argument("--name", default="", help="specific object name")

    sub.add_parser("uscript-export",
                   help="UnrealScript: decompile .U packages to .uc source (UE Explorer replacement)")

    sp = sub.add_parser("uscript", help="UnrealScript: grep the decompiled .uc source tree")
    sp.add_argument("pattern", help="regex (case-insensitive)")

    return p


def main():
    parser = build_parser()
    a = parser.parse_args()

    if a.cmd == "decompile":
        return cmd_ghidra("decompile", [a.target])
    if a.cmd == "func":
        return cmd_ghidra("func", [a.pattern])
    if a.cmd == "struct":
        return cmd_ghidra("struct", [a.name])
    if a.cmd == "xref":
        return cmd_ghidra("xref", [a.name])
    if a.cmd == "search":
        return cmd_ghidra("search", [a.text])
    if a.cmd == "strings":
        return cmd_ghidra("strings", [a.substr])
    if a.cmd == "data":
        return cmd_ghidra("data", [a.addr, str(a.count)])
    if a.cmd == "umodel-list":
        return cmd_umodel_list(a.package)
    if a.cmd == "umodel-export":
        return cmd_umodel_export(a.package, a.type, a.name)
    if a.cmd == "uscript-export":
        return cmd_uscript_export()
    if a.cmd == "uscript":
        return cmd_uscript_grep(a.pattern, None)
    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
