"""Run all regression checks through one CTest entry."""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sim", "rv32i", "rv32a", "csr", "memory"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    checks = [
        ("rv32i", [args.rv32i]),
        ("rv32a", [args.rv32a]),
        ("csr", [args.csr]),
        ("memory", [args.memory]),
        ("trace_tools", [sys.executable, str(tests / "test_trace_tools.py")]),
        ("integration", [sys.executable, str(tests / "test_integration.py"),
                          args.sim]),
        ("loader", [sys.executable, str(tests / "test_loader.py"), args.sim]),
        ("spike_tools", [sys.executable, str(tests / "test_spike_tools.py")]),
    ]
    failed = []
    for name, command in checks:
        print(f"Running {name}", flush=True)
        try:
            subprocess.run(command, check=True, timeout=60)
        except (OSError, subprocess.SubprocessError) as error:
            print(f"FAILED {name}: {error}", flush=True)
            failed.append(name)
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed", flush=True)
    if failed:
        print(f"Failed checks: {', '.join(failed)}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
