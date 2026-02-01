#!/usr/bin/env -S uv run --python 3.14 --script
# /// script
# requires-python = ">=3.14"
# ///
"""Compile-test Sky1 patches against various kernel config targets.

Usage:
  build-test.py                     # default: allyesconfig
  build-test.py allyesconfig        # everything =y
  build-test.py allmodconfig        # everything =m
  build-test.py defconfig           # ARM64 default
  build-test.py randconfig          # random config
  build-test.py all                 # allyesconfig + allmodconfig

These are compile-only smoke tests — the resulting kernels won't boot.
They catch API mismatches and #ifdef bugs when our code interacts with
options we don't normally enable.

When to run:
  - First rebase of a new kernel series (e.g. 6.19-rc1, 6.20-rc1)
  - Before promotion (rc -> latest)
Not worth running for point releases or RC bumps within a series.
"""

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

VALID_TARGETS = ["allyesconfig", "allmodconfig", "defconfig", "randconfig", "all"]


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, **kwargs)


def get_branch() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        capture_output=True, text=True, check=True,
    )
    return result.stdout.strip()


def get_base_info() -> str:
    """Get base tag/ref info for display."""
    branch = get_branch()
    result = subprocess.run(
        ["git", "describe", "--tags", "--abbrev=0", "HEAD"],
        capture_output=True, text=True, check=False,
    )
    base = result.stdout.strip() if result.returncode == 0 else "unknown"
    result = subprocess.run(
        ["git", "rev-list", "--count", f"{base}..HEAD"],
        capture_output=True, text=True, check=False,
    )
    count = result.stdout.strip() if result.returncode == 0 else "?"
    return f"{branch} ({base} + {count} patches)"


def count_config_options(path: Path) -> int:
    return sum(1 for line in path.read_text().splitlines() if line.startswith("CONFIG_"))


def build_target(target: str, warnings: bool, modules_only: bool) -> bool:
    """Run a single build test. Returns True on success."""
    jobs = os.cpu_count() or 4
    config_path = Path(".config")

    print(f"\n>>> Generating {target}...")
    run(["make", "ARCH=arm64", target])

    option_count = count_config_options(config_path)
    print(f"    {option_count} config options")

    make_cmd = ["make", "ARCH=arm64", f"-j{jobs}"]
    if warnings:
        make_cmd.append("W=1")

    targets = ["modules"] if modules_only else ["Image", "modules"]
    make_cmd.extend(targets)

    target_desc = " + ".join(targets)
    print(f">>> Building {target_desc}...")

    start = time.monotonic()
    result = subprocess.run(make_cmd, capture_output=True, text=True)
    elapsed = time.monotonic() - start
    minutes = int(elapsed // 60)
    seconds = int(elapsed % 60)

    if result.returncode == 0:
        print(f"  [PASS] {target} build succeeded ({minutes}m {seconds}s)")
        return True
    else:
        print(f"  [FAIL] {target} build failed ({minutes}m {seconds}s)")
        # Show last error lines
        stderr_lines = result.stderr.strip().splitlines()
        error_lines = [l for l in stderr_lines if "error:" in l.lower()]
        if error_lines:
            print()
            for line in error_lines[-10:]:
                print(f"  {line}")
        elif stderr_lines:
            print()
            for line in stderr_lines[-10:]:
                print(f"  {line}")
        return False


def main() -> None:
    import argparse
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("target", nargs="?", default="allyesconfig",
                        choices=VALID_TARGETS,
                        help="Config target (default: allyesconfig)")
    parser.add_argument("--warnings", "-W", action="store_true",
                        help="Enable W=1 for extra compiler warnings")
    parser.add_argument("--modules-only", action="store_true",
                        help="Only build modules, skip Image")
    parser.add_argument("--keep-config", action="store_true",
                        help="Don't restore .config after test")
    parser.add_argument("--repeat", "-n", type=int, default=1,
                        help="Iterations for randconfig (default: 1)")
    args = parser.parse_args()

    os.chdir(Path(__file__).resolve().parent.parent)

    # Save current config
    config_path = Path(".config")
    backup_path = Path(".config.build-test-backup")

    if config_path.exists():
        shutil.copy2(config_path, backup_path)
        print(f"Saved .config to {backup_path}")
    else:
        print("Warning: no .config found (nothing to restore)")

    branch_info = get_base_info()

    if args.target == "all":
        targets = ["allyesconfig", "allmodconfig"]
    elif args.target == "randconfig":
        targets = ["randconfig"] * args.repeat
    else:
        targets = [args.target]

    print(f"\n=== Sky1 Build Test ===")
    print(f"Branch:  {branch_info}")
    print(f"Targets: {', '.join(targets)}")

    results: dict[str, bool] = {}
    for i, target in enumerate(targets):
        label = f"{target}[{i+1}]" if target == "randconfig" and len(targets) > 1 else target
        # Clean between targets
        if i > 0:
            print(f"\n>>> Cleaning between targets...")
            run(["make", "ARCH=arm64", "clean"])
        results[label] = build_target(target, args.warnings, args.modules_only)

    # Restore config
    if not args.keep_config and backup_path.exists():
        print(f"\n>>> Restoring .config...")
        shutil.copy2(backup_path, config_path)
        backup_path.unlink()

    # Final report
    print(f"\n=== Results ===")
    all_passed = True
    for label, passed in results.items():
        status = "PASS" if passed else "FAIL"
        print(f"  {label:20s} [{status}]")
        if not passed:
            all_passed = False

    if all_passed:
        print("\nAll build tests passed.")
    else:
        print("\nSome build tests FAILED.")
        sys.exit(1)


if __name__ == "__main__":
    main()
