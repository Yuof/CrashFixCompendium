#!/usr/bin/env python3
"""Two checks of the source that ctest runs, for mistakes the compiler can't see:
- Every src/fixes/<Key>.cpp registers itself under the key its file is named after and has a line in the shipped INI,
  in the section it registers (.section, default Fixes), and every INI line has a fix: a fix with no line in its
  section stays off at runtime, so a mismatch ships a fix nobody can turn on.
- Nothing logged below error level says error / fail / failed / failure: a scan of the log for them would flag a clean
  run.
  py Tools/check_source.py"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BAD_WORD = re.compile(r"\b(error|failed|failure|fail)\b", re.I)
# a log call below error level, or a Result that goes to one, up to the end of its statement
BELOW_ERROR = re.compile(r"(?:logger::(?:info|warn)\s*\(|Outcome::k(?:Active|NotNeeded)\b).*?;", re.S)
LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')


def registered_key(text):
    match = re.search(r'Register\(\{\s*\.key\s*=\s*"(\w+)"', text)
    return match.group(1) if match else None


def registered_section(text):
    match = re.search(r'Register\(\{[^}]*?\.section\s*=\s*"(\w+)"', text, re.S)
    return match.group(1) if match else "Fixes"


def ini_keys(text):
    """{key: section} of the INI's active lines."""
    keys, section = {}, None
    for line in text.splitlines():
        if m := re.match(r"\s*\[(\w+)\]", line):
            section = m.group(1)
        elif m := re.match(r"(\w+)\s*=", line):
            keys[m.group(1)] = section
    return keys


def key_problems(files, ini, sections=None):
    """files maps each fix file's stem to the key it registers, or None; ini maps INI keys to their section;
    sections maps a stem to the section it registers (default Fixes)."""
    sections = sections or {}
    stems = set(files)
    fixes = sorted(files.items())
    if isinstance(ini, dict):
        wrong = [f"{k}: in [{ini[k]}] of the shipped INI, registers [{sections.get(k, 'Fixes')}]"
                 for k in sorted(stems & set(ini)) if ini[k] != sections.get(k, "Fixes")]
    else:
        wrong = []
    ini = set(ini)
    return [f"{stem}.cpp: registers no fix" for stem, key in fixes if key is None] + \
           [f"{stem}.cpp: registers the key {key}" for stem, key in fixes if key not in (None, stem)] + \
           [f"{k}: a fix file, no line in the shipped INI" for k in sorted(stems - ini)] + \
           [f"{k}: in the shipped INI, no fix file" for k in sorted(ini - stems)] + wrong


def wording_problems(name, text):
    return [f"{name}: {literal} is logged below error level" for statement in BELOW_ERROR.findall(text)
            for literal in LITERAL.findall(statement) if BAD_WORD.search(literal)]


def main():
    texts = {p.stem: p.read_text("utf-8") for p in (ROOT / "src" / "fixes").glob("*.cpp")}
    files = {stem: registered_key(t) for stem, t in texts.items()}
    sections = {stem: registered_section(t) for stem, t in texts.items()}
    ini = ini_keys((ROOT / "dist" / "SKSE" / "Plugins" / "CrashFixCompendium.ini").read_text("utf-8"))
    problems = key_problems(files, ini, sections)
    for path in sorted((ROOT / "src").rglob("*.cpp")):
        problems += wording_problems(path.name, path.read_text("utf-8"))
    for p in problems:
        print("FAIL", p)
    if problems:
        return 1
    print(f"source OK ({len(files)} fixes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
