#!/usr/bin/env python3
"""Check archive boundaries and declarations of exported functions."""

import os
import re
import subprocess
import sys
from pathlib import Path


def defined_symbols(archive):
    output = subprocess.check_output(
        [os.environ.get("NM", "nm"), "-A", "-g", "--defined-only",
         "--format=posix", archive],
        text=True,
    )
    for line in output.splitlines():
        match = re.fullmatch(
            r"[^[]+\[([^]]+\.o)\]: (\S+) ([A-Za-z])(?: .*)?",
            line,
        )
        if match is None:
            raise ValueError(f"unexpected nm output: {line}")
        yield match.group(1), match.group(2), match.group(3)


def undefined_symbols(archive):
    output = subprocess.check_output(
        [os.environ.get("NM", "nm"), "-A", "-u", "--format=posix", archive], text=True
    )
    for line in output.splitlines():
        match = re.fullmatch(
            r"[^[]+\[([^]]+\.o)\]: (\S+) U\s*", line
        )
        if match is None:
            raise ValueError(f"unexpected nm output: {line}")
        yield match.group(1), match.group(2)


def project_symbol(symbol):
    return symbol.startswith(("s3_", "log_")) or symbol == "secure_free"


def main():
    s3_archive, log_archive = sys.argv[1:]
    s3_headers = "\n".join(
        Path(name).read_text()
        for name in (
            "s3.h",
            "s3_log.h",
            "s3_internal.h",
            "s3_xml.h",
            "s3_upload.h",
            "secure_free.h",
        )
    )
    log_header = Path("log.h").read_text()
    errors = []
    for archive, header in ((s3_archive, s3_headers), (log_archive, log_header)):
        found = False
        for member, symbol, kind in defined_symbols(archive):
            if kind.upper() not in ("T", "W"):
                continue
            if not project_symbol(symbol):
                print(f"info: {archive}[{member}]: other function {symbol}")
                continue
            found = True
            if archive == s3_archive:
                module = member.removesuffix(".o")
                if symbol != module and not symbol.startswith(module + "_"):
                    errors.append(f"{member}: unexpected symbol {symbol}")
            elif member != "log.o" or not symbol.startswith("log_"):
                errors.append(f"{member}: unexpected symbol {symbol}")
            if re.search(r"\b" + re.escape(symbol) + r"\s*\(", header) is None:
                errors.append(f"{symbol}: no header declaration")
        if not found:
            errors.append(f"{archive}: no defined project functions")
    for member, symbol in undefined_symbols(s3_archive):
        if symbol.startswith(("s3ar_", "main_", "sig_")):
            errors.append(f"{member}: application dependency {symbol}")
    for member, symbol in undefined_symbols(log_archive):
        if symbol.startswith(("s3_", "s3ar_", "main_", "sig_")):
            errors.append(f"{member}: application or S3 dependency {symbol}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("archive symbol boundaries OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
