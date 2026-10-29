#!/usr/bin/env python3
"""Checks src/plugins/ClapAbi.h (the self-written CLAP declarations) against the official CLAP headers: every struct's size
and field offsets, the extension ids and the constants. Skipped when the headers (the `clap` package) aren't installed.

    python3 tests/clap_abi_check.py
"""
import os, re, subprocess, sys, tempfile

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
abi = os.path.join(root, "src", "plugins", "ClapAbi.h")


def generate():
    src = open(abi).read()
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    def fields(body):
        names, depth, cur, parts = [], 0, "", []
        for ch in body:
            if ch == "{": depth += 1
            if ch == "}": depth -= 1
            if ch == ";" and depth == 0: parts.append(cur); cur = ""
            else: cur += ch
        for d in parts:
            d = d.strip()
            if not d: continue
            if d.startswith("union"): names += fields(d[d.index("{")+1:d.rindex("}")]); continue
            if "(*" in d: names.append(d.split("(*")[1].split(")")[0].strip()); continue
            d = re.sub(r"\[[^\]]*\]", "", d)
            first, *rest = d.split(",")
            names.append(re.findall(r"\w+", first)[-1]); names += [re.findall(r"\w+", r)[-1] for r in rest]
        return names
    structs = [(m.group(1), fields(m.group(2))) for m in re.finditer(r"struct (\w+) \{(.*?)\n\};", src, flags=re.S)]
    strings = [m.group(1) for m in re.finditer(r"constexpr char (\w+)\[\] = \"([^\"]*)\";", src)]
    values = [m.group(1) for m in re.finditer(r"\b(CLAP_[A-Z0-9_]+) = ([^,\n;]+)[,\n;]", src) if m.group(1) != "CLAP_VERSION"]
    o = ['#include "plugins/ClapAbi.h"', "#include <cstddef>", "#include <cstdio>", "#include <cstring>"]
    o.append("struct Lay { const char* what; size_t v; };")
    o.append("static const Lay ours_lay[] = {")
    for n, fs in structs:
        o.append(f'  {{"sizeof {n}", sizeof(clap::{n})}},')
        for f in fs: o.append(f'  {{"{n}.{f}", offsetof(clap::{n}, {f})}},')
    o.append("};")
    o.append("static const char* ours_str[] = {" + ", ".join(f"clap::{s}" for s in strings) + "};")
    o.append("static const long long ours_val[] = {" + ", ".join(f"(long long)clap::{v}" for v in values) + "};")
    o.append("#include <clap/clap.h>")
    o.append("static const size_t official_lay[] = {")
    for n, fs in structs:
        o.append(f"  sizeof(::{n}_t),")
        for f in fs: o.append(f"  offsetof(::{n}_t, {f}),")
    o.append("};")
    def ref(n): return f"\n#ifdef {n}\n {n}\n#else\n ::{n}\n#endif\n"
    o.append("static const char* official_str[] = {" + ",".join(ref(s) for s in strings) + "};")
    o.append("static const long long official_val[] = {" + ",".join(f"(long long)({ref(v)})" for v in values) + "};")
    o.append('static const char* str_names[] = {' + ", ".join(f'"{s}"' for s in strings) + "};")
    o.append('static const char* val_names[] = {' + ", ".join(f'"{v}"' for v in values) + "};")
    o.append("""int main() {
        int bad = 0;
        for (size_t i = 0; i < sizeof(ours_lay) / sizeof(*ours_lay); ++i)
            if (ours_lay[i].v != official_lay[i]) { std::printf("LAYOUT %s: ours %zu, official %zu\\n", ours_lay[i].what, ours_lay[i].v, official_lay[i]); ++bad; }
        for (size_t i = 0; i < sizeof(ours_str) / sizeof(*ours_str); ++i)
            if (std::strcmp(ours_str[i], official_str[i])) { std::printf("STRING %s: ours %s, official %s\\n", str_names[i], ours_str[i], official_str[i]); ++bad; }
        for (size_t i = 0; i < sizeof(ours_val) / sizeof(*ours_val); ++i)
            if (ours_val[i] != official_val[i]) { std::printf("VALUE %s: ours %lld, official %lld\\n", val_names[i], ours_val[i], official_val[i]); ++bad; }
        std::printf("checked %zu struct layout entries, %zu strings, %zu values: %d mismatch(es)\\n", sizeof(ours_lay) / sizeof(*ours_lay),
            sizeof(ours_str) / sizeof(*ours_str), sizeof(ours_val) / sizeof(*ours_val), bad);
        return bad != 0;
    }""")
    return "\n".join(o) + "\n", len(structs)


if not os.path.exists("/usr/include/clap/clap.h"):
    print("skip: no official CLAP headers (/usr/include/clap)")
    sys.exit(0)
code, n = generate()
with tempfile.TemporaryDirectory() as d:
    cpp, exe = os.path.join(d, "check.cpp"), os.path.join(d, "check")
    open(cpp, "w").write(code)
    subprocess.run(["c++", "-std=c++17", "-I" + os.path.join(root, "src"), cpp, "-o", exe], check=True)
    sys.exit(subprocess.run([exe]).returncode)
