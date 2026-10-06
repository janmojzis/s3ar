"""Exercise the archive check with real compiler and nm output."""

import os
from pathlib import Path
import shlex
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "tests" / "check_archive_symbols.py"


def archive(tmp_path, name, member, source, pic):
    path = tmp_path / (member + ".c")
    path.write_text(source)
    obj = path.with_suffix(".o")
    flags = ["-fPIC"] if pic else ["-fno-pic", "-fno-pie"]
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), *flags, "-c",
                    str(path), "-o", str(obj)], check=True, capture_output=True)
    result = tmp_path / name
    subprocess.run([os.environ.get("AR", "ar"), "rcs", str(result), str(obj)],
                   check=True, capture_output=True)
    return result


def check(tmp_path, source, member="s3_client", log_source="void log_set_name(void) {}",
          pic=True):
    s3 = archive(tmp_path, "libs3.a", member, source, pic)
    log = archive(tmp_path, "liblog.a", "log", log_source, pic)
    return subprocess.run([sys.executable, str(CHECK), str(s3), str(log)],
                          cwd=ROOT, capture_output=True, text=True)


@pytest.mark.parametrize("register", ["ax", "bx", "cx", "dx", "si", "di", "bp"])
def test_archive_symbols_accept_i386_pic_helpers(tmp_path, register):
    # Also exercise all helper names on hosts where the compiler emits none.
    result = check(tmp_path, f'''
        void s3_client_close(void) {{}}
        void helper(void) __asm__("__x86.get_pc_thunk.{register}");
        void helper(void) {{}}
    ''', pic=False)
    assert result.returncode == 0, result.stderr
    assert "archive symbol boundaries OK" in result.stdout


def test_archive_symbols_accept_compiler_generated_pic(tmp_path):
    result = check(tmp_path, """
        extern void external(void);
        void s3_client_close(void) { external(); }
    """)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("attribute", ["", "__attribute__((weak))"])
def test_archive_symbols_reject_wrong_module_exports(tmp_path, attribute):
    result = check(tmp_path, f"{attribute} void s3_client_close(void) {{}}",
                   member="s3_error")
    assert result.returncode == 1
    assert "s3_error.o: unexpected symbol s3_client_close" in result.stderr


@pytest.mark.parametrize("symbol", ["s3_client_undeclared", "s3_client.undeclared"])
def test_archive_symbols_reject_undeclared_exports(tmp_path, symbol):
    result = check(tmp_path, f'void helper(void) __asm__("{symbol}"); void helper(void) {{}}')
    assert result.returncode == 1
    assert f"{symbol}: no header declaration" in result.stderr


@pytest.mark.parametrize("symbol", ["__x86.get_pc_thunk.other", "_helper", "foreign_helper"])
def test_archive_symbols_report_other_exports(tmp_path, symbol):
    result = check(tmp_path, f'''
        void s3_client_close(void) {{}}
        void helper(void) __asm__("{symbol}");
        void helper(void) {{}}
    ''')
    assert result.returncode == 0, result.stderr
    assert f"other function {symbol}" in result.stdout
    assert "archive symbol boundaries OK" in result.stdout


@pytest.mark.parametrize("library", ["s3", "log"])
def test_archive_symbols_reject_application_dependencies(tmp_path, library):
    source = "extern void s3ar_die(int); void s3_client_close(void) { s3ar_die(2); }"
    log_source = "extern void s3ar_die(int); void log_set_name(void) { s3ar_die(2); }"
    result = check(tmp_path, source if library == "s3" else "void s3_client_close(void) {}",
                   log_source=log_source if library == "log" else "void log_set_name(void) {}")
    assert result.returncode == 1
    assert "dependency s3ar_die" in result.stderr


def test_archive_symbols_require_project_functions(tmp_path):
    result = check(tmp_path, "void foreign_helper(void) {}")
    assert result.returncode == 1
    assert "no defined project functions" in result.stderr
