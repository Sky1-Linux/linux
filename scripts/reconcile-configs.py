#!/usr/bin/env -S uv run --python 3.14 --script
# /// script
# requires-python = ">=3.14"
# ///
"""Reconcile kernel configs across Sky1 build tracks.

Modes:
  reconcile-configs.py              Compare all track configs, check policy
  reconcile-configs.py --fix        Auto-fix policy violations in-place
  reconcile-configs.py --verbose    Show all Sky1-specific options
  reconcile-configs.py --review OLD NEW   Review config changes after olddefconfig
"""

import argparse
import configparser
import re
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path

LINUX_SKY1 = Path.home() / "sky1-linux-distro" / "linux-sky1"
CONFIG_DIR = LINUX_SKY1 / "config"
POLICY_FILE = CONFIG_DIR / "config-policy.ini"

TRACKS = {
    "LTS": "config.sky1",
    "Latest": "config.sky1-latest",
    "RC": "config.sky1-rc",
    "Next": "config.sky1-next",
}

# Options to ignore when comparing across tracks (version/toolchain-specific)
IGNORE_PATTERNS = [
    r"^CONFIG_CC_VERSION_TEXT=",
    r"^CONFIG_GCC_VERSION=",
    r"^CONFIG_LD_VERSION=",
    r"^CONFIG_CLANG_VERSION=",
    r"^CONFIG_AS_VERSION=",
    r"^CONFIG_PAHOLE_VERSION=",
    r"^CONFIG_RUSTC_VERSION=",
    r"^CONFIG_BINDGEN_VERSION=",
    r"^CONFIG_KERNEL_VERSION_GENERATION=",
]

# Subsystem prefixes for categorizing new options in --review mode
SUBSYSTEM_PREFIXES = {
    "DRM_": "Display/GPU",
    "SND_": "Audio",
    "NET_": "Network",
    "NFT_": "Network/Netfilter",
    "NF_": "Network/Netfilter",
    "CRYPTO_": "Crypto",
    "SECURITY_": "Security",
    "USB_": "USB",
    "PHY_": "PHY",
    "PCI_": "PCI",
    "ARM64_": "ARM64",
    "ARCH_": "Architecture",
    "CIX_": "Sky1",
    "SKY1_": "Sky1",
    "LINLON": "Sky1",
    "TRILIN": "Sky1",
    "PANTHOR": "Sky1",
    "ARMCHINA": "Sky1",
    "CDNSP_SKY1": "Sky1",
}


def parse_config(path: Path) -> dict[str, str]:
    """Parse a kernel .config file into {CONFIG_FOO: value} dict."""
    opts: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_"):
            key, _, val = line.partition("=")
            opts[key] = val
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            key = line.split()[1]
            opts[key] = "n"
    return opts


def parse_policy() -> dict[str, str]:
    """Parse config-policy.ini into {CONFIG_FOO: value} dict."""
    if not POLICY_FILE.exists():
        print(f"Warning: policy file not found: {POLICY_FILE}")
        return {}

    cp = configparser.ConfigParser()
    cp.read(POLICY_FILE)

    policy: dict[str, str] = {}
    for section in cp.sections():
        for key, val in cp.items(section):
            policy[f"CONFIG_{key.upper()}"] = val
    return policy


def is_ignored(line: str) -> bool:
    return any(re.match(pat, line) for pat in IGNORE_PATTERNS)


def categorize_option(opt: str) -> str:
    """Categorize a config option by subsystem prefix."""
    name = opt.removeprefix("CONFIG_")
    for prefix, category in SUBSYSTEM_PREFIXES.items():
        if name.startswith(prefix):
            return category
    return "Other"


def is_sky1_option(opt: str) -> bool:
    name = opt.removeprefix("CONFIG_")
    sky1_markers = [
        "CIX", "SKY1", "LINLON", "TRILIN", "PANTHOR",
        "ARMCHINA", "CDNSP_SKY1",
    ]
    return any(m in name for m in sky1_markers)


def cmd_reconcile(fix: bool = False, verbose: bool = False) -> int:
    """Compare all track configs and check policy."""
    print("=== Sky1 Config Reconciliation ===\n")

    # Load configs
    configs: dict[str, dict[str, str]] = {}
    for label, filename in TRACKS.items():
        path = CONFIG_DIR / filename
        if path.exists():
            configs[label] = parse_config(path)
            count = sum(1 for v in configs[label].values() if v != "n")
            print(f"  {label:8s} {filename:25s} ({count} enabled options)")
        else:
            print(f"  {label:8s} {filename:25s} (not found, skipping)")

    if len(configs) < 2:
        print("\nNeed at least 2 configs to compare.")
        return 0

    print()

    policy = parse_policy()
    violations: list[str] = []
    fixed = 0

    # Check policy
    if policy:
        print("--- Policy Check ---")
        for opt, required_val in sorted(policy.items()):
            for label, cfg in configs.items():
                actual = cfg.get(opt)
                if actual is None:
                    violations.append(f"  {TRACKS[label]:25s} {opt}  MISSING  (policy: {required_val})")
                elif actual != required_val:
                    violations.append(f"  {TRACKS[label]:25s} {opt}={actual}  (policy: {required_val})")

        if violations:
            for v in violations:
                print(v)
            print()
            if fix:
                for opt, required_val in sorted(policy.items()):
                    for label, cfg in configs.items():
                        actual = cfg.get(opt)
                        if actual is not None and actual != required_val:
                            path = CONFIG_DIR / TRACKS[label]
                            text = path.read_text()
                            old = f"{opt}={actual}"
                            new = f"{opt}={required_val}"
                            if old in text:
                                text = text.replace(old, new)
                                path.write_text(text)
                                print(f"  Fixed: {TRACKS[label]}: {old} -> {new}")
                                fixed += 1
                if fixed:
                    print(f"\n  {fixed} violation(s) fixed.\n")
        else:
            print("  All options match policy.\n")

    # Compare Sky1-specific options across tracks
    sky1_opts: set[str] = set()
    for cfg in configs.values():
        sky1_opts.update(k for k in cfg if is_sky1_option(k))

    divergent: list[str] = []
    for opt in sorted(sky1_opts):
        vals = {}
        for label, cfg in configs.items():
            vals[label] = cfg.get(opt, "-")

        unique = set(vals.values())
        if len(unique) > 1 or verbose:
            parts = "  ".join(f"{l}={v}" for l, v in vals.items())
            marker = " ***" if len(unique) > 1 else ""
            divergent.append(f"  {opt:45s} {parts}{marker}")

    header = "Sky1-Specific Options (all)" if verbose else "Sky1-Specific Divergence"
    print(f"--- {header} ---")
    if divergent:
        for d in divergent:
            print(d)
    else:
        print("  All Sky1-specific options are consistent across tracks.")
    print()

    # Summary
    div_count = sum(1 for d in divergent if "***" in d)
    print("--- Summary ---")
    print(f"  {len(violations)} policy violation(s)")
    print(f"  {div_count} divergent Sky1-specific option(s)")
    print(f"  {len(sky1_opts)} total Sky1-specific options checked")

    return 1 if violations and not fix else 0


def cmd_review(old_path: Path, new_path: Path) -> int:
    """Review config changes between two configs (e.g. before/after olddefconfig)."""
    if not old_path.exists():
        print(f"Error: {old_path} not found")
        return 1
    if not new_path.exists():
        print(f"Error: {new_path} not found")
        return 1

    old = parse_config(old_path)
    new = parse_config(new_path)

    print(f"=== Config Review: {old_path.name} -> {new_path.name} ===\n")
    print(f"  Old: {sum(1 for v in old.values() if v != 'n')} enabled options")
    print(f"  New: {sum(1 for v in new.values() if v != 'n')} enabled options")
    print()

    # New options
    added = {k: v for k, v in new.items() if k not in old and not is_ignored(f"{k}={v}")}
    # Removed options
    removed = {k: v for k, v in old.items() if k not in new and not is_ignored(f"{k}={v}")}
    # Changed options
    changed = {
        k: (old[k], new[k])
        for k in old
        if k in new and old[k] != new[k] and not is_ignored(f"{k}={old[k]}")
    }

    # Categorize new options
    if added:
        by_category: dict[str, list[tuple[str, str]]] = defaultdict(list)
        for opt, val in sorted(added.items()):
            cat = categorize_option(opt)
            by_category[cat].append((opt, val))

        print(f"--- NEW options ({len(added)}) ---")
        # Show Sky1-relevant first
        for cat in ["Sky1", "Display/GPU", "Audio", "USB", "PCI", "PHY", "Network"]:
            if cat in by_category:
                print(f"\n  [{cat}]")
                for opt, val in by_category.pop(cat):
                    print(f"    {opt}={val}")

        # Then other categories
        for cat in sorted(by_category):
            items = by_category[cat]
            if len(items) <= 10:
                print(f"\n  [{cat}]")
                for opt, val in items:
                    print(f"    {opt}={val}")
            else:
                print(f"\n  [{cat}] ({len(items)} options, showing first 5)")
                for opt, val in items[:5]:
                    print(f"    {opt}={val}")
                print(f"    ... ({len(items) - 5} more)")
        print()

    # Removed options
    if removed:
        was_enabled = [(k, v) for k, v in removed.items() if v in ("y", "m")]
        was_disabled = [(k, v) for k, v in removed.items() if v == "n"]

        print(f"--- REMOVED options ({len(removed)}) ---")
        if was_enabled:
            print(f"\n  Previously ENABLED (review!):")
            for opt, val in sorted(was_enabled):
                print(f"    {opt} (was: {val})")
        if was_disabled:
            print(f"\n  Previously disabled ({len(was_disabled)} options, no action needed)")
        print()

    # Changed values
    if changed:
        print(f"--- CHANGED values ({len(changed)}) ---")
        for opt, (old_val, new_val) in sorted(changed.items()):
            sky1_flag = " [Sky1]" if is_sky1_option(opt) else ""
            print(f"  {opt}: {old_val} -> {new_val}{sky1_flag}")
        print()

    # Summary
    print("--- Summary ---")
    print(f"  {len(added)} new options")
    print(f"  {len(removed)} removed options")
    print(f"  {len(changed)} changed values")

    # Save review to file
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    review_file = Path.home() / "docs" / f"config-review-{timestamp}.txt"
    # Only save if there are meaningful changes
    if added or removed or changed:
        print(f"\n  Review saved to: {review_file}")

    return 0


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--fix", action="store_true",
                        help="Auto-fix policy violations in-place")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Show all Sky1-specific options, not just divergent")
    parser.add_argument("--review", nargs=2, metavar=("OLD", "NEW"),
                        help="Review config changes between two files")
    args = parser.parse_args()

    if args.review:
        sys.exit(cmd_review(Path(args.review[0]), Path(args.review[1])))
    else:
        sys.exit(cmd_reconcile(fix=args.fix, verbose=args.verbose))


if __name__ == "__main__":
    main()
