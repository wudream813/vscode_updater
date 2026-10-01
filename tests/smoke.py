"""Offline CLI tests: no network requests or installations are performed."""
import pathlib
import subprocess
import sys

exe = str(pathlib.Path(sys.argv[1]).resolve())
cases = [
    (["--help"], 0),
    (["--unknown"], 2),
    (["--threads"], 2),
    (["--threads", "abc"], 2),
    (["--threads", "0"], 2),
    (["--threads", "17"], 2),
    (["--arch", "x86"], 2),
    (["--dir", "--force"], 2),
        (["--stream", "--help"], 0),
    (["--no-stream", "--help"], 0),
    (["--check", "--dir", "."], 1),  # refused before querying the network
]
for args, expected in cases:
    result = subprocess.run([exe, *args], capture_output=True, timeout=15)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    assert b"\x1b" not in result.stdout + result.stderr, "ANSI escapes leaked into redirected output"
print(f"PASS: {len(cases)} offline CLI checks (including no-pause and exit codes)")
