#!/usr/bin/env python3
"""Catch Windows paths in the documents whose backslash escape has been eaten.

Why this exists. Writing `docs\\formats\\rmd3.md` into a file through a shell heredoc can drop the
backslash, and whatever reads the text afterwards turns `\\f` and `\\r` into a form feed and a carriage
return. The path stops being a path, silently - the document still looks fine in a diff viewer that
renders control characters as nothing.

So this is not a style check. A documented command that cannot be copied and run is wrong in the same
way a failing test is wrong.

What it flags, and nothing else:
  * BEL, BS, VT or FF anywhere in a text document - these are `\\a`, `\\b`, `\\v`, `\\f` that lost their
    backslash, and none of them belongs in Markdown;
  * a TAB with a word character on both sides - `\\t`, never indentation;
  * a line break between a path prefix (`docs`, `src`, `tools`, `work`, `tests`, `build_*`) and what
    looks like the tail of a filename - `\\r` or `\\n`.

Usage:
    python tools\\scout\\docpaths.py verify
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FILES = sorted(n for n in os.listdir(ROOT) if n.lower().endswith(".md"))
DIRS = ["docs", "tests", "db", "re"]
EXTENSIONS = (".md", ".ps1", ".py", ".txt", ".json", ".yaml")

CONTROL = {"\a": "\\a", "\b": "\\b", "\v": "\\v", "\f": "\\f"}
PREFIX = re.compile(r"(tests|tools|src|docs|work|db|re|build_\w+)$")
TAIL = re.compile(r"^[a-z][\w.]*\.(ps1|py|cpp|h|md|txt|bin|exe|json|png|obj)\b")
TAB_IN_TOKEN = re.compile(r"\w\t\w")


def documents():
    for name in FILES:
        yield os.path.join(ROOT, name)
    for d in DIRS:
        for base, _, names in os.walk(os.path.join(ROOT, d)):
            for name in names:
                if name.lower().endswith(EXTENSIONS):
                    yield os.path.join(base, name)


def main():
    if len(sys.argv) > 1 and sys.argv[1] not in ("verify",):
        print(__doc__)
        return 2
    checked = 0
    findings = []
    for path in documents():
        try:
            text = io.open(path, encoding="utf-8", newline="").read()
        except (UnicodeDecodeError, OSError):
            continue
        checked += 1
        rel = os.path.relpath(path, ROOT)
        for ch, escape in CONTROL.items():
            if ch in text:
                findings.append("%s: %d x %r, i.e. a lost %s" % (rel, text.count(ch), ch, escape))
        for i, line in enumerate(text.split("\n")):
            if TAB_IN_TOKEN.search(line):
                findings.append("%s:%d: TAB inside a token, i.e. a lost \\t" % (rel, i + 1))
        lines = text.split("\n")
        for i in range(len(lines) - 1):
            if PREFIX.search(lines[i].rstrip("\r")) and TAIL.match(lines[i + 1]):
                findings.append("%s:%d: a path broken across lines, i.e. a lost \\r or \\n" % (rel, i + 1))
    for f in findings:
        print("  " + f)
    print("docpaths: %d documents, %d broken path(s)" % (checked, len(findings)))
    return 0 if not findings else 1


if __name__ == "__main__":
    sys.exit(main())
