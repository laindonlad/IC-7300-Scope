#!/usr/bin/env python3
"""The interface's English text, for translating, and how complete each translation in lang/ is.

    python tools/lang_strings.py              writes lang/template.txt.in: every string, with empty translations
    python tools/lang_strings.py --check      says what each lang/*.txt is missing, and what it has that's gone

The strings are what the source passes to tr(), trId(), trf() and N_(), and the labels of the program's own
buttons (panelButton, toggle and so on), which translate themselves. A lang file is pairs of lines: the English,
then "= " and the translation, with \\n for a new line. A translation must keep the English's %s, %d and so on,
in the same order, or the program keeps the English.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
LANG = os.path.join(ROOT, "lang")

# Every literal inside these calls is the English for a translation.
TRANSLATED = {"tr", "trId", "trf", "N_"}
# These take a label as their first argument and translate it where it's shown.
LABELLED = {"panelButton", "toggle", "actionButton", "levelRow", "switchLevelRow", "radioRow", "comboTr", "heading", "tab",
            "label", "pbt"}

TOKEN = re.compile(r'''
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:\\.|[^"\\\n])*")
  | (?P<char>'(?:\\.|[^'\\\n])+')
  | (?P<ident>[A-Za-z_][A-Za-z_0-9]*(?:::[A-Za-z_][A-Za-z_0-9]*)*)
  | (?P<punct>[(),{}\[\]])
  | (?P<other>.)
''', re.S | re.X)


def unescape(lit):
    body = lit[1:-1]
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            if n == "x":
                j = i + 2
                while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(bytes([int(body[i + 2:j], 16)]).decode("latin-1"))
                i = j
                continue
            out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'"}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strings_in(path):
    text = open(path, encoding="utf-8").read()
    if os.path.basename(path) == "app.cpp":
        # The self-test's own text, which is always English.
        a, b = text.find("void App::expect("), text.find("void App::runSelfTest(")
        if a >= 0 and b > a:
            text = text[:a] + text[b:]
    found = []
    stack = []  # (callee, argument index) for each open bracket
    last_ident = None
    pending = []  # adjacent literals, which C joins into one
    pending_ctx = None

    def flush():
        nonlocal pending, pending_ctx
        if pending:
            found.append(("".join(pending), pending_ctx))
        pending, pending_ctx = [], None

    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind == "comment" or (kind == "other" and m.group().isspace()):
            continue
        if kind == "string":
            ctx = tuple(stack[-1]) if stack else None
            if pending and ctx != pending_ctx:
                flush()
            pending.append(unescape(m.group()))
            pending_ctx = ctx
            last_ident = None
            continue
        flush()
        if kind == "ident":
            last_ident = m.group().split("::")[-1]
            continue
        tok = m.group()
        if tok == "(":
            stack.append([last_ident, 0])
        elif tok in "[{":
            stack.append([None, 0])
        elif tok in ")]}":
            if stack:
                stack.pop()
        elif tok == "," and stack:
            stack[-1][1] += 1
        last_ident = None
    flush()

    keep = []
    for s, ctx in found:
        if not ctx:
            continue
        callee, arg = ctx
        # Only the format of a trf(); the rest are what fills it.
        if callee in TRANSLATED and not (callee == "trf" and arg > 0):
            pass
        elif callee in LABELLED and arg == 0:
            # Front-panel abbreviations stay as the rig has them: ATU, SPLIT, ARM TX and so on.
            shown = s.split("##")[0]
            if not re.search(r"[a-z]", shown) and len(shown) <= 6 or shown == "ARM TX":
                continue
        else:
            continue
        s = s.split("##")[0] if callee != "tr" and callee != "trf" else s
        if not re.search(r"[A-Za-z]{2}", s):
            continue
        keep.append(s)
    return keep


def all_strings():
    seen, out = set(), []
    for path in sorted(glob.glob(os.path.join(SRC, "*.cpp"))):
        for s in strings_in(path):
            if s not in seen:
                seen.add(s)
                out.append((s, os.path.basename(path)))
    return out


def escape(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n")


def read_lang(path):
    pairs, key = {}, None
    for line in open(path, encoding="utf-8-sig").read().split("\n"):
        line = line.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        if line.startswith("="):
            if key is not None:
                v = line[1:]
                pairs[key] = v[1:] if v.startswith(" ") else v
            key = None
        else:
            key = line
    return pairs


def shape(s):
    return re.findall(r"%[-+ #0']*(\*?)\d*(?:\.(\*?)\d*)?([hlLzjtq]*)([diouxXeEfFgGaAcspn%])", s)


def main():
    strings = all_strings()
    if "--check" in sys.argv:
        keys = {escape(s) for s, _ in strings}
        for path in sorted(glob.glob(os.path.join(LANG, "*.txt"))):
            have = read_lang(path)
            missing = [k for k in keys if k not in have or not have[k]]
            gone = [k for k in have if k not in keys]
            bad = [k for k in have if k in keys and have[k] and [x for x in shape(k) if x[3] != "%"] != [x for x in shape(have[k]) if x[3] != "%"]]
            print(f"{os.path.basename(path)}: {len(keys) - len(missing)} of {len(keys)} translated, {len(gone)} no longer used, {len(bad)} with % fields that don't match")
            for k in bad:
                print("  % fields differ: " + k)
            if "-v" in sys.argv:
                for k in missing:
                    print("  missing: " + k)
                for k in gone:
                    print("  unused: " + k)
        return
    out = os.path.join(LANG, "template.txt.in")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Every string in the interface. Copy this to xx.txt and put the translation after each \"= \".\n")
        f.write("# Keep %s, %d and the like, in the same order. \\n is a new line.\n\n")
        for s, where in strings:
            f.write(f"{escape(s)}\n= \n\n")
    print(f"{len(strings)} strings written to {out}")


if __name__ == "__main__":
    main()
