"""Run the real scanner/probe/host against an explicitly built, pinned SDK fixture."""
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote


def main() -> None:
    if len(sys.argv) not in (5, 6):
        raise ValueError("Expected scanner, host test, runtime helper, bundle and optional 'instrument'")
    instrument = len(sys.argv) == 6
    if instrument and sys.argv[5] != "instrument":
        raise ValueError("Unknown SDK fixture kind")
    expected_name = "mda DX10" if instrument else "ADelay"
    expected_flag = "1" if instrument else "0"
    scanner, host, runtime, raw_plugin = sys.argv[1:5]
    plugin = Path(raw_plugin).resolve(strict=True)
    result = subprocess.run([scanner, "--list-module", str(plugin)], check=True,
                            capture_output=True, text=True, timeout=15)
    rows = [line.split("\t") for line in result.stdout.splitlines() if line]
    candidates = [row for row in rows if len(row) == 7 and unquote(row[3]) == expected_name]
    if len(candidates) != 1:
        raise ValueError(f"Expected one real {expected_name} processor, got {result.stdout!r}")
    row = candidates[0]
    if not re.fullmatch(r"[0-9A-F]{32}", row[0]):
        raise ValueError("Scanner returned an invalid class FUID")
    if not re.fullmatch(r"[0-9A-F]{64}", unquote(row[2])) or row[6] != expected_flag:
        raise ValueError("Scanner returned invalid fingerprint or fixture metadata")
    if Path(unquote(row[1])).resolve() != plugin:
        raise ValueError("Scanner returned a different module path")
    probe = subprocess.run([scanner, "--probe", str(plugin), row[0]], check=True,
                           capture_output=True, text=True, timeout=15)
    if probe.stdout.strip() != "ok":
        raise ValueError("Real component probe did not acknowledge initialization")
    print(f"PASS: real SDK scanner + probe, {expected_name} class={row[0]}", flush=True)
    subprocess.run([host, str(plugin), row[0], runtime, unquote(row[2])], check=True, timeout=60 if instrument else 30)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            # Preserve sanitizer/error diagnostics emitted by the real scanner.
            for output in (error.stdout, error.stderr):
                if output:
                    print(output[-8000:], file=sys.stderr)
        raise SystemExit(1)
