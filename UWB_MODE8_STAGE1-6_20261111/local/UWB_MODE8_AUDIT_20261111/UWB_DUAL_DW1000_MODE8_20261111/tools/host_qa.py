#!/usr/bin/env python3
"""Reproduce HOST-only QA. Syntax stubs do not emulate ESP32 timing or link SDK."""
from __future__ import annotations
import json
import os
from pathlib import Path
import subprocess
import sys
import shutil
from sync_shared import sync
ROOT=Path(__file__).resolve().parents[1]

def main() -> int:
    ev=ROOT/"evidence/host";ev.mkdir(parents=True,exist_ok=True)
    build=ROOT/"build/host";build.mkdir(parents=True,exist_ok=True)
    sync()
    results=[]
    def run(name,cmd):
        log=ev/(name+".log")
        with log.open("w") as f:
            f.write("HOST-ONLY COMMAND: "+json.dumps(cmd)+"\n");f.flush()
            p=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
        results.append({"name":name,"command":cmd,"exit_code":p.returncode,"log":str(log.relative_to(ROOT))})
        if p.returncode:raise RuntimeError(f"{name} failed: {log}")
    try:
        cxx=shutil.which("g++")
        if not cxx:raise RuntimeError("g++ required for host QA")
        run("compiler_version",[cxx,"--version"])
        run("core_compile",[cxx,"-std=c++11","-O1","-g","-Wall","-Wextra","-Wpedantic","-Werror","-Ishared","-fsanitize=address,undefined","tests/test_core.cpp","-o",str(build/"test_core")])
        run("core_test",[str(build/"test_core"),str(ev)])
        for role in ("anchor","tag"):
            run(role+"_syntax_compile",[cxx,"-std=c++11","-DESP32","-DARDUINO","-Wall","-Wextra","-Wpedantic","-Werror","-Itests/stubs",f"-Ivendor/{role}/DW1000Ng/src","-c",f"firmware/{role.upper()}_DUAL_DS_CIR/app.cpp","-o",str(build/(role+"_syntax_ONLY.o"))])
        clang=shutil.which("clang++")
        if clang:
            clang_ev=ev/"clang_fixture";clang_ev.mkdir(exist_ok=True)
            run("clang_version",[clang,"--version"])
            run("clang_core_compile",[clang,"-std=c++11","-Wall","-Wextra","-Wpedantic","-Werror","-Ishared","tests/test_core.cpp","-o",str(build/"test_core_clang")])
            run("clang_core_test",[str(build/"test_core_clang"),str(clang_ev)])
            for role in ("anchor","tag"):
                run("clang_"+role+"_syntax",[clang,"-std=c++11","-DESP32","-DARDUINO","-Wall","-Wextra","-Wpedantic","-Werror","-Itests/stubs",f"-Ivendor/{role}/DW1000Ng/src","-fsyntax-only",f"firmware/{role.upper()}_DUAL_DS_CIR/app.cpp"])
        run("python_tools_tests",[sys.executable,"-m","unittest","discover","-s","tests","-p","test_tools.py","-v"])
        run("python_bytecode",[sys.executable,"-m","compileall","-q","tools","tests"])
        run("shared_headers",[sys.executable,"tools/sync_shared.py","--check"])
        status="PASS"
    except Exception as e:
        status="FAIL";results.append({"error":str(e)})
    report={"status":status,"scope":"HOST_ONLY", "target_build":False,
            "hardware_tested":False,"sdk":"hand-written declaration stubs for syntax checks only",
            "dw1000_headers":"actual unmodified role-specific vendored headers", "results":results}
    (ev/"host_qa_receipt.json").write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
    return 0 if status=="PASS" else 1

if __name__=="__main__":sys.exit(main())
