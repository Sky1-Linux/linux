#!/usr/bin/env -S uv run --python 3.14 --script
# /// script
# requires-python = ">=3.14"
# ///
"""Query upstream git repos and compare to local branch state.

Usage: ./scripts/kernel-track-status.py [--fetch] [--json]

Checks:
  - LTS point releases (stable remote, v6.18.x tags)
  - Latest stable releases (stable remote)
  - RC tags (origin remote, v6.X-rcN)
  - RC graduation (origin, release tag without -rc)
"""

import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sky1_lib

# Colors (disabled if not a terminal)
if sys.stdout.isatty():
    RED, YELLOW, GREEN, CYAN, NC = (
        "\033[0;31m", "\033[0;33m", "\033[0;32m", "\033[0;36m", "\033[0m",
    )
else:
    RED = YELLOW = GREEN = CYAN = NC = ""


def version_sort_key(tag: str) -> tuple:
    """Sort key for version tags like v6.18.7, v6.19-rc7."""
    s = tag.lstrip("v")
    parts = re.split(r"[-.]", s)
    result: list[tuple[int, int]] = []
    for p in parts:
        if p.startswith("rc"):
            # RC versions sort before release
            result.append((0, int(p[2:])))
        else:
            try:
                result.append((1, int(p)))
            except ValueError:
                result.append((0, 0))
    return tuple(result)


def latest_remote_tag(remote: str, pattern: str) -> str | None:
    """Get latest upstream tag matching a pattern from a remote."""
    result = sky1_lib.git("ls-remote", "--tags", remote, pattern, check=False)
    if result.returncode != 0 or not result.stdout.strip():
        return None

    tags: list[str] = []
    for line in result.stdout.strip().split("\n"):
        if "\t" not in line:
            continue
        ref = line.split("\t")[1]
        tag = ref.removeprefix("refs/tags/").removesuffix("^{}")
        if tag:
            tags.append(tag)

    if not tags:
        return None

    return sorted(set(tags), key=version_sort_key)[-1]


def main() -> None:
    os.chdir(Path(__file__).resolve().parent.parent)

    import argparse
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fetch", action="store_true",
                        help="Fetch needed tags before checking")
    parser.add_argument("--json", action="store_true",
                        help="Machine-readable JSON output")
    parser.parse_args()

    print(f"{CYAN}=== Sky1 Kernel Track Status ==={NC}")
    print()

    actions: list[tuple[str, str]] = []

    # --- LTS track (main branch) ---
    print(f"{'LTS (main):':<12s}", end="")
    if sky1_lib.branch_exists("main"):
        ver = sky1_lib.get_kernel_version("main")
        local_lts = ver.full
        lts_mm = ver.major_minor
        print(f"local={local_lts:<14s}", end="")

        upstream_lts = latest_remote_tag("stable", f"refs/tags/v{lts_mm}.*")
        if upstream_lts:
            print(f"upstream={upstream_lts:<14s}", end="")
            local_sub = local_lts.split(".")[2] if len(local_lts.split(".")) > 2 else "0"
            upstream_sub = upstream_lts.removeprefix(f"v{lts_mm}.")
            if local_sub != upstream_sub:
                print(f"{YELLOW}UPDATE AVAILABLE{NC}")
                actions.append((
                    f"Rebase main onto {upstream_lts}",
                    f"git checkout main && git rebase --onto {upstream_lts} v{local_lts}",
                ))
            else:
                print(f"{GREEN}up to date{NC}")
        else:
            print("upstream=unknown")
    else:
        print(f"{RED}branch missing{NC}")

    # --- Latest track (latest branch) ---
    print(f"{'Latest:':<12s}", end="")
    if sky1_lib.branch_exists("latest"):
        ver = sky1_lib.get_kernel_version("latest")
        local_latest = ver.full
        latest_mm = ver.major_minor
        print(f"local={local_latest:<14s}", end="")

        upstream_latest = latest_remote_tag("stable", f"refs/tags/v{latest_mm}.*")
        if upstream_latest:
            print(f"upstream={upstream_latest:<14s}", end="")
            local_sub = local_latest.split(".")[2] if len(local_latest.split(".")) > 2 else "0"
            upstream_sub = upstream_latest.removeprefix(f"v{latest_mm}.")
            if local_sub != upstream_sub:
                print(f"{YELLOW}UPDATE AVAILABLE{NC}")
                actions.append((
                    f"Rebase latest onto {upstream_latest}",
                    f"git checkout latest && git rebase --onto {upstream_latest} v{local_latest}",
                ))
            else:
                print(f"{GREEN}up to date{NC}")
        else:
            print("upstream=unknown")
    else:
        print("not started", end="")
        # Check if RC has graduated
        if sky1_lib.branch_exists("rc"):
            ver = sky1_lib.get_kernel_version("rc")
            rc_mm = ver.major_minor
            release_tag = latest_remote_tag("origin", f"refs/tags/v{rc_mm}")
            if release_tag:
                stable_tag = latest_remote_tag("stable", f"refs/tags/v{rc_mm}.*")
                tag = stable_tag or release_tag
                print(f"   upstream={tag:<10s}", end="")
                print(f"{YELLOW}NEW RELEASE — promote rc to latest{NC}")
                actions.append((
                    f"Promote rc -> latest ({release_tag} released)",
                    f"git checkout -b latest rc && git rebase --onto {tag} v{ver.full}",
                ))
            else:
                print()
        else:
            print()

    # --- RC track (rc branch) ---
    print(f"{'RC (rc):':<12s}", end="")
    if sky1_lib.branch_exists("rc"):
        ver = sky1_lib.get_kernel_version("rc")
        local_rc = ver.full
        rc_mm = ver.major_minor
        print(f"local={local_rc:<14s}", end="")

        # Check if graduated
        release_tag = latest_remote_tag("origin", f"refs/tags/v{rc_mm}")
        if release_tag and "-rc" in local_rc:
            print(f"upstream={release_tag:<14s}", end="")
            print(f"{YELLOW}GRADUATED — promote to latest{NC}")
        else:
            latest_rc = latest_remote_tag("origin", f"refs/tags/v{rc_mm}-rc*")
            if latest_rc:
                print(f"upstream={latest_rc:<14s}", end="")
                if f"v{local_rc}" != latest_rc:
                    print(f"{YELLOW}NEWER RC AVAILABLE{NC}")
                    actions.append((
                        f"Rebase rc onto {latest_rc}",
                        f"git checkout rc && git rebase --onto {latest_rc} v{local_rc}",
                    ))
                else:
                    print(f"{GREEN}up to date{NC}")
            else:
                print("upstream=unknown")

        # Check for next RC cycle (could be minor bump or major bump, e.g. 6.19 -> 7.0)
        candidates = [
            f"{ver.major}.{ver.minor + 1}",  # e.g. 6.20
            f"{ver.major + 1}.0",             # e.g. 7.0
        ]
        for next_mm in candidates:
            next_rc1 = latest_remote_tag("origin", f"refs/tags/v{next_mm}-rc1")
            if next_rc1:
                actions.append((
                    f"New RC cycle available ({next_rc1})",
                    f"git checkout rc && git rebase --onto {next_rc1} v{local_rc}",
                ))
                break
    else:
        print(f"{RED}branch missing{NC}")

    # --- Next track (next branch) ---
    print(f"{'Next:':<12s}", end="")
    if sky1_lib.branch_exists("next"):
        ver = sky1_lib.get_kernel_version("next")
        print(f"local={ver.full:<14s}", end="")
        print(f"{GREEN}active{NC}")
    else:
        print("not started")

    print()

    if actions:
        print(f"{CYAN}Recommended actions:{NC}")
        for i, (desc, cmd) in enumerate(actions):
            print(f"  {i + 1}. {YELLOW}{desc}{NC}")
            print(f"     {cmd}")
        print()
        print("After any rebase, run:  ./scripts/update-dev-boot.py")
        print("Then build and test:    ./scripts/build-install.py")
    else:
        print(f"{GREEN}All tracks up to date.{NC}")


if __name__ == "__main__":
    main()
