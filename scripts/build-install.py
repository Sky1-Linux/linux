#!/usr/bin/env -S uv run --python 3.14 --script
# /// script
# requires-python = ">=3.14"
# ///
"""Build and install mainline kernel for Sky1 dev systems.

Usage:
  ./scripts/build-install.py              # auto-detect track from branch
  ./scripts/build-install.py --track rc   # explicit track (for feature branches)
  ./scripts/build-install.py clean        # clean build first

Auto-detects:
  - Board (from /sys/firmware/devicetree/base/compatible)
  - Track (main/latest/rc/next) from branch name, or --track override
  - Kernel version (from Makefile)

Derives EFI image and DTB filenames accordingly.
"""

import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sky1_lib


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    """Run a command, checking return code by default."""
    return subprocess.run(cmd, check=True, **kwargs)


def main() -> None:
    os.chdir(Path(__file__).resolve().parent.parent)

    import argparse
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", nargs="?", default=None,
                        choices=["clean"],
                        help="'clean' to do a clean build first")
    parser.add_argument("--track", "-t", default=None,
                        choices=sky1_lib.BRANCHES,
                        help="kernel track (default: auto-detect from branch)")
    args = parser.parse_args()

    board = sky1_lib.detect_board()

    # Detect branch
    result = sky1_lib.git("rev-parse", "--abbrev-ref", "HEAD")
    branch = result.stdout.strip()
    if not branch:
        print("Error: Not in a git repository")
        sys.exit(1)

    # Resolve track: explicit --track overrides branch-based auto-detect
    track = args.track or branch
    if track not in sky1_lib.BRANCHES:
        print(f"Error: Cannot determine track from branch '{branch}'")
        print(f"       Use --track {{{','.join(sky1_lib.BRANCHES)}}}")
        sys.exit(1)

    # Get version from current HEAD
    ver = sky1_lib.get_kernel_version("HEAD")

    # Derive EFI filenames using track (not raw branch name)
    efi = sky1_lib.get_efi_names(track, ver.major_minor, board.dtb_prefix)

    efi_dir = Path("/boot/efi")
    dts_src = Path(f"arch/arm64/boot/dts/cix/{board.dts_base}.dtb")
    jobs = os.cpu_count() or 4

    print("=== Sky1 Kernel Build ===")
    print(f"Board:   {board.compat} ({board.dts_base})")
    print(f"Branch:  {branch}" + (f" (track: {track})" if track != branch else ""))
    print(f"Version: {ver.full}")
    print(f"Image:   {efi_dir / efi.image}")
    print(f"DTB:     {efi_dir / efi.dtb}")
    print(f"Jobs:    {jobs}")

    # Check for config drift vs release config
    config_map = {
        "main": "config.sky1",
        "latest": "config.sky1-latest",
        "rc": "config.sky1-rc",
        "next": "config.sky1-next",
    }
    release_cfg_name = config_map.get(track)
    if release_cfg_name:
        release_cfg = (Path.home() / "sky1-linux-distro" / "linux-sky1"
                       / "config" / release_cfg_name)
        local_cfg = Path(".config")
        if release_cfg.exists() and local_cfg.exists():
            local_opts = {
                l.split("=", 1)[0]
                for l in local_cfg.read_text().splitlines()
                if l.startswith("CONFIG_")
            }
            release_opts = {
                l.split("=", 1)[0]
                for l in release_cfg.read_text().splitlines()
                if l.startswith("CONFIG_")
            }
            # Count lines that differ (ignoring toolchain version strings)
            local_lines = {
                l for l in local_cfg.read_text().splitlines()
                if l.startswith("CONFIG_") and not l.startswith("CONFIG_CC_VERSION")
                and not l.startswith("CONFIG_GCC_VERSION")
                and not l.startswith("CONFIG_LD_VERSION")
            }
            release_lines = {
                l for l in release_cfg.read_text().splitlines()
                if l.startswith("CONFIG_") and not l.startswith("CONFIG_CC_VERSION")
                and not l.startswith("CONFIG_GCC_VERSION")
                and not l.startswith("CONFIG_LD_VERSION")
            }
            diff_count = len(local_lines.symmetric_difference(release_lines))
            if diff_count:
                print(f"\n  NOTE: .config differs from {release_cfg_name}"
                      f" in {diff_count} options")
                print(f"        Run ./scripts/reconcile-configs.py for details")

    print()

    if args.action == "clean":
        print(">>> Cleaning build...")
        run(["make", "clean"])
        print()

    # Ensure generated headers match current tree (prevents vermagic mismatch
    # when switching branches without a full rebuild)
    kernel_release_file = Path("include/config/kernel.release")
    if kernel_release_file.exists():
        cached = kernel_release_file.read_text().strip()
        expected = subprocess.run(
            ["make", "-s", "kernelrelease"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        if cached != expected:
            print(f">>> Vermagic stale: '{cached}' != '{expected}'")
            print(">>> Running modules_prepare to regenerate headers...")
            run(["make", f"-j{jobs}", "modules_prepare"])
            print()

    # Build kernel, modules, and device trees
    print(">>> Building kernel, modules, and DTBs...")
    run(["make", f"-j{jobs}", "Image", "modules", "dtbs"])

    print()
    print(">>> Installing modules...")
    run(["sudo", "make", "modules_install"])

    print()
    print(">>> Copying kernel image to EFI...")
    src_image = Path("arch/arm64/boot/Image")
    dst_image = efi_dir / efi.image
    run(["sudo", "cp", str(src_image), str(dst_image)])

    print(">>> Copying DTB to EFI...")
    dst_dtb = efi_dir / efi.dtb
    run(["sudo", "cp", str(dts_src), str(dst_dtb)])

    print(">>> Syncing EFI filesystem...")
    run(["sync"])

    # Verify copies by comparing file sizes
    src_size = src_image.stat().st_size
    dst_size = dst_image.stat().st_size
    if src_size != dst_size:
        print("ERROR: Kernel image size mismatch!")
        print(f"  Source: {src_size} bytes")
        print(f"  Dest:   {dst_size} bytes")
        sys.exit(1)

    src_size = dts_src.stat().st_size
    dst_size = dst_dtb.stat().st_size
    if src_size != dst_size:
        print("ERROR: DTB file size mismatch!")
        print(f"  Source: {src_size} bytes")
        print(f"  Dest:   {dst_size} bytes")
        sys.exit(1)

    # Get kernel release string
    result = subprocess.run(
        ["make", "-s", "kernelrelease"],
        capture_output=True, text=True, check=True,
    )
    kernelrelease = result.stdout.strip()

    print()
    print("=== Build Complete ===")
    print(f"Kernel:  {dst_image}")
    print(f"DTB:     {dst_dtb}")
    print(f"Modules: /lib/modules/{kernelrelease}")
    print()
    run(["ls", "-la", str(dst_image), str(dst_dtb)])
    print()
    print("Verified: EFI files match build artifacts.")
    print("Reboot and select the matching GRUB entry to test.")
    print("Run ./scripts/update-dev-boot.py if GRUB entries need updating.")


if __name__ == "__main__":
    main()
