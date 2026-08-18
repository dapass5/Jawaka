#!/usr/bin/env python3
"""Extract Leaf's translatable strings into i18n/leaf.pot.

Why not xgettext: `--keyword=T` only sees strings written inside T(...), and
most of Leaf's UI text is not. Labels are literals at funnel call sites
(jw__render_list_row(..., "Auto Sleep", ...)) that a shared renderer translates,
footer hints live in cat_footer_item initializers, and a handful of label
arrays (tabs, system names) are translated where they are indexed. An extractor
that does not know those shapes reports a fraction of the real universe, and
the coverage gate would gate on the wrong total.

This extractor knows exactly four shapes, listed in FUNNELS / ARRAYS below.
When a new funnel is added, add it here -- the CI check will not notice its
strings otherwise. That is the maintenance contract, and it is spelled out in
umrk-workspace/plans/leaf-i18n-chinese.md.

Usage:
    tools/i18n-extract.py                 # rewrite i18n/leaf.pot
    tools/i18n-extract.py --check         # fail if the committed .pot is stale
    tools/i18n-extract.py --check --po i18n/zh_CN.po
                                          # ...and fail on orphan keys in the .po
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POT = ROOT / "i18n" / "leaf.pot"

# Sources that draw UI. The i18n test's T() calls are fixtures, not UI.
SOURCE_GLOBS = ["cmd/**/*.c", "internal/**/*.c"]
EXCLUDE_PARTS = ("third_party", "cmd/jawaka-i18n-test")

# Funnel calls whose string-literal arguments are all UI text. The extractor
# takes every double-quoted literal inside the call's parentheses; each of
# these was checked by hand to carry no non-UI literals at any call site.
FUNNELS = [
    "T",
    "jw__render_list_row",
    "jw__render_list_row_h",
    "jw__render_nav_row",
    "jw__draw_header",
    "jw__draw_slider_row",
    "jw__render_account_row",
    "jw__draw_info_title",
    "jw__about_push",
]

# A run of adjacent string literals -- C concatenates "a" "b" into one string,
# and several dialog messages span lines that way. Every extraction site below
# must treat the run as ONE key or the runtime key never matches.
STR = r'"(?:[^"\\]|\\.)*"'
RUN = re.compile(STR + r"(?:\s*" + STR + r")*")


def run_to_text(run: str) -> str:
    return "".join(m.group(1) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', run))


# Struct-initializer shapes. Positional footers ({ CAT_BTN_X, "Label" }) and the
# designated-init dialogs (.message = "...", .label = "...") are both UI text --
# verified by hand against every occurrence in the tree.
FOOTER_RE = re.compile(r"\{\s*CAT_BTN_[A-Z0-9_]+\s*,\s*(" + RUN.pattern + r")")
DESIG_RE  = re.compile(r"\.(?:message|label)\s*=\s*(" + RUN.pattern + r")")

# Label arrays translated where they are indexed (T(kTabs[i]) and the like).
# kSystemDisplayNames rows are {"ID", "Display Name"}; only the name is UI.
ARRAYS = [
    ("cmd/jawaka-launcher/main.c", "kTabs", "all"),
    ("cmd/jawaka-launcher/main.c", "kSysMenuTabs", "all"),
    ("cmd/jawaka-launcher/main.c", "kSysActions", "all"),
    ("cmd/jawaka-launcher/main.c", "kSysInfo", "all"),
    ("cmd/jawaka-menu/main.c", "kInGameItems", "all"),
    ("internal/settings/settings.c", "kHomeCategoryLabels", "all"),
    ("internal/settings/settings.c", "kStartupTabLabels", "all"),
    ("internal/settings/settings.c", "kAutoSleepLabels", "all"),
    ("internal/launcher/system_names.c", "kSystemDisplayNames", "second"),
    ("internal/settings/settings.c", "kTimeZones", "first"),
    ("cmd/jawaka-menu/main.c", "kCpuPerfOptions", "first"),
    ("cmd/jawaka-menu/main.c", "kGpuPerfOptions", "first"),
    ("cmd/jawaka-menu/main.c", "kDmcPerfOptions", "first"),
]

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def c_unescape(s: str) -> str:
    return (s.replace(r"\n", "\n").replace(r"\t", "\t")
             .replace(r"\"", '"').replace(r"\\", "\\"))


def strip_comments(src: str) -> str:
    """Drop comments so a commented-out call is not extracted. String contents
    are preserved (comment markers inside string literals survive because the
    scanner tracks quoting)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and (src[j] != '"' or src[j - 1] == "\\"):
                j += 1
            out.append(src[i:j + 1]); i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c); i += 1
    return "".join(out)


def call_spans(src: str, name: str):
    """Yield the text between the parentheses of each `name(...)` call."""
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", src):
        depth, j = 1, m.end()
        while j < len(src) and depth:
            c = src[j]
            if c == '"':
                j += 1
                while j < len(src) and (src[j] != '"' or src[j - 1] == "\\"):
                    j += 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            j += 1
        yield src[m.end():j - 1]


def array_literals(src: str, name: str, mode: str):
    m = re.search(re.escape(name) + r"[^=]*=\s*\{", src)
    if not m:
        return
    depth, j = 1, m.end()
    while j < len(src) and depth:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
        j += 1
    body = src[m.end():j - 1]
    if mode == "all":
        for lit in STRING_RE.findall(body):
            yield lit
    else:  # struct rows: "second" takes {"ID","Name"}, "first" takes {"Label",...}
        idx = 1 if mode == "second" else 0
        for row in re.finditer(r"\{([^{}]*)\}", body):
            lits = STRING_RE.findall(row.group(1))
            if len(lits) > idx:
                yield lits[idx]


def extract():
    keys = {}

    def add(lit, where):
        text = c_unescape(lit)
        if not text or text == "%s":
            return
        keys.setdefault(text, where)

    files = []
    for pattern in SOURCE_GLOBS:
        files.extend(ROOT.glob(pattern))
    for f in sorted(set(files)):
        rel = f.relative_to(ROOT).as_posix()
        if any(part in rel for part in EXCLUDE_PARTS):
            continue
        src = strip_comments(f.read_text(encoding="utf-8", errors="replace"))
        for fn in FUNNELS:
            for span in call_spans(src, fn):
                for m in RUN.finditer(span):
                    add(run_to_text(m.group(0)), rel)
        for m in FOOTER_RE.finditer(src):
            add(run_to_text(m.group(1)), rel)
        for m in DESIG_RE.finditer(src):
            add(run_to_text(m.group(1)), rel)
        for arr_file, arr_name, mode in ARRAYS:
            if rel == arr_file:
                for lit in array_literals(src, arr_name, mode):
                    add(lit, rel)
    return keys


def write_pot(keys, path: Path):
    lines = [
        "# Leaf UI strings. GENERATED by tools/i18n-extract.py -- do not edit;",
        "# regenerate with `make i18n-pot`. The English string is the key, and",
        "# a context prefix (\"verb|Open\") is part of the key.",
        'msgid ""',
        'msgstr "Content-Type: text/plain; charset=UTF-8\\n"',
        "",
    ]
    for key in sorted(keys):
        esc = key.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        lines.append(f"#: {keys[key]}")
        lines.append(f'msgid "{esc}"')
        lines.append('msgstr ""')
        lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def pot_keys(path: Path):
    out = set()
    for m in re.finditer(r'^msgid "((?:[^"\\]|\\.)*)"', path.read_text(encoding="utf-8"), re.M):
        if m.group(1):
            out.add(c_unescape(m.group(1)))
    return out


def po_entries(path: Path):
    """(all keys, translated keys). A key with an empty msgstr is present but
    untranslated -- it must count for the orphan check and NOT for coverage,
    or a fully-seeded file reads as 100% before anyone has reviewed a word."""
    text = path.read_text(encoding="utf-8")
    all_keys, translated = set(), set()
    ctx = None
    entry_re = re.compile(
        r'^(msgctxt|msgid|msgstr) "((?:[^"\\]|\\.)*)"', re.M)
    last_key = None
    for m in entry_re.finditer(text):
        kind, val = m.group(1), c_unescape(m.group(2))
        if kind == "msgctxt":
            ctx = val
        elif kind == "msgid":
            last_key = (f"{ctx}|{val}" if ctx else val) if val else None
            if last_key:
                all_keys.add(last_key)
            ctx = None
        elif kind == "msgstr" and last_key:
            if val:
                translated.add(last_key)
            last_key = None
    return all_keys, translated


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail if i18n/leaf.pot is stale instead of rewriting it")
    ap.add_argument("--po", nargs="*", default=[],
                    help="translation files to validate against the extracted set")
    args = ap.parse_args()

    keys = extract()
    print(f"extracted {len(keys)} keys")

    rc = 0
    if args.check:
        if not POT.exists():
            print(f"FAIL: {POT} does not exist; run tools/i18n-extract.py", file=sys.stderr)
            return 1
        committed = pot_keys(POT)
        fresh = set(keys)
        missing = sorted(fresh - committed)
        stale = sorted(committed - fresh)
        if missing or stale:
            for k in missing[:20]:
                print(f"  not in committed .pot: {k!r}", file=sys.stderr)
            if len(missing) > 20:
                print(f"  ... and {len(missing) - 20} more", file=sys.stderr)
            for k in stale[:20]:
                print(f"  in .pot but no longer in code: {k!r}", file=sys.stderr)
            if len(stale) > 20:
                print(f"  ... and {len(stale) - 20} more", file=sys.stderr)
            print("FAIL: i18n/leaf.pot is stale; run `make i18n-pot` and commit it",
                  file=sys.stderr)
            rc = 1
        else:
            print("i18n/leaf.pot is current")
    else:
        write_pot(keys, POT)
        print(f"wrote {POT.relative_to(ROOT)}")

    universe = set(keys)
    for po in args.po:
        p = Path(po)
        if not p.exists():
            continue
        pk, translated = po_entries(p)
        orphans = sorted(pk - universe)
        if orphans:
            for k in orphans[:20]:
                print(f"  {p.name}: key not in the code: {k!r}", file=sys.stderr)
            print(f"FAIL: {p.name} has {len(orphans)} orphan key(s) -- an English "
                  "string was probably renamed; msgmerge the translation forward",
                  file=sys.stderr)
            rc = 1
        else:
            covered = len(translated & universe)
            print(f"{p.name}: {covered}/{len(universe)} translated "
                  f"({covered * 100 // max(1, len(universe))}%)")
    return rc


if __name__ == "__main__":
    sys.exit(main())
