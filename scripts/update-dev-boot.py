#!/usr/bin/env -S uv run --python 3.14 --script
# /// script
# requires-python = ">=3.14"
# ///
"""Auto-generate GRUB dev entries from local kernel branches.

Usage: ./scripts/update-dev-boot.py [-n] [--force]

Discovers kernel versions from main/latest/rc/next branches,
detects the current board, and regenerates the dev entries section
of grub.cfg between ### BEGIN/END SKY1-DEV-ENTRIES ### markers.

Content outside the markers is never touched.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sky1_lib

GRUB_CFG = Path("/boot/efi/GRUB/grub.cfg")
BEGIN_MARKER = "### BEGIN SKY1-DEV-ENTRIES"
END_MARKER = "### END SKY1-DEV-ENTRIES ###"
ROOT_PARTUUID = "c0fae6e2-4dbb-4cc1-8a8b-532904dbf5c8"

# Edit boot parameters here — backslashes and formatting are preserved exactly.
ENTRY_TEMPLATE = """\
menuentry '{title}' {{
    devicetree /{dtb}
    linux /{image} \\
        loglevel=7 \\
        console=tty0 \\
        console=ttyAMA2,115200 \\
        efi=noruntime \\
        earlycon=efifb \\
        earlycon=pl011,0x040d0000 \\
        acpi=off \\
        clk_ignore_unused \\
        linlon_dp.enable_fb=1 \\
        linlon_dp.enable_render=0 \\
        fbcon=map:01111111 \\
        keep_bootcon \\
        panic=30 \\
        root=PARTUUID={partuuid} rootwait rw
}}"""


def emit_entry(title: str, dtb: str, image: str) -> str:
    """Generate a single GRUB menuentry."""
    return ENTRY_TEMPLATE.format(
        title=title,
        dtb=dtb,
        image=image,
        partuuid=ROOT_PARTUUID,
    )


def generate_entries(board: sky1_lib.Board) -> str:
    """Generate all dev entries for existing branches."""
    entries: list[str] = []
    entry_num = 0

    for branch in sky1_lib.BRANCHES:
        if not sky1_lib.branch_exists(branch):
            continue

        ver = sky1_lib.get_kernel_version(branch)
        efi = sky1_lib.get_efi_names(branch, ver.major_minor, board.dtb_prefix)

        label = sky1_lib.TRACK_LABELS[branch]
        if branch == "next":
            title = f"{entry_num} Sky1 next dev (NVMe)"
        elif label:
            title = f"{entry_num} Sky1 {ver.major_minor} {label} dev (NVMe)"
        else:
            title = f"{entry_num} Sky1 {ver.major_minor} dev (NVMe)"

        entries.append(emit_entry(title, efi.dtb, efi.image))
        entry_num += 1

    return "\n\n".join(entries)


def content_hash(content: str) -> str:
    """Compute first 16 hex chars of SHA-256 hash."""
    return hashlib.sha256(content.encode()).hexdigest()[:16]


def main() -> None:
    os.chdir(Path(__file__).resolve().parent.parent)

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="Print what would be generated, don't write")
    parser.add_argument("--force", action="store_true",
                        help="Override hash validation (allow overwriting manual edits)")
    args = parser.parse_args()

    board = sky1_lib.detect_board()
    new_content = generate_entries(board)

    if not new_content:
        print("Error: No kernel branches found (main/latest/rc/next)")
        sys.exit(1)

    new_hash = content_hash(new_content)

    print("=== Sky1 Dev Boot Entries ===")
    print(f"Board: {board.compat}")
    print(f"Hash:  {new_hash}")
    print()

    # Show what will be generated
    for branch in sky1_lib.BRANCHES:
        if sky1_lib.branch_exists(branch):
            ver = sky1_lib.get_kernel_version(branch)
            efi = sky1_lib.get_efi_names(branch, ver.major_minor, board.dtb_prefix)
            print(f"  {branch:<8s} {ver.full:<16s} -> {efi.image} / {efi.dtb}")
    print()

    if args.dry_run:
        print("--- Generated entries (dry-run) ---")
        print(new_content)
        return

    if not GRUB_CFG.exists():
        print(f"Error: {GRUB_CFG} not found")
        sys.exit(1)

    cfg_text = GRUB_CFG.read_text()

    # Check if markers exist
    has_markers = BEGIN_MARKER in cfg_text
    if has_markers:
        # Extract current content between markers
        lines = cfg_text.split("\n")
        in_section = False
        current_lines: list[str] = []
        old_hash: str | None = None

        for line in lines:
            if line.startswith(BEGIN_MARKER):
                in_section = True
                m = re.search(r"[a-f0-9]{16}", line)
                old_hash = m.group(0) if m else None
                continue
            if line.startswith(END_MARKER):
                in_section = False
                continue
            if in_section:
                current_lines.append(line)

        current_content = "\n".join(current_lines)
        current_hash = content_hash(current_content)

        if old_hash and old_hash != current_hash and not args.force:
            print("Error: Dev entries were manually edited since last update.")
            print(f"  Expected hash: {old_hash}")
            print(f"  Current hash:  {current_hash}")
            print()
            print(f"Use --force to override, or edit {GRUB_CFG} manually.")
            sys.exit(1)

        if current_hash == new_hash and old_hash == new_hash:
            print(f"GRUB entries are up to date (hash: {new_hash}). No changes needed.")
            return

    # Create timestamped backup
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = GRUB_CFG.with_name(f"{GRUB_CFG.name}.backup-{timestamp}")
    subprocess.run(["sudo", "cp", str(GRUB_CFG), str(backup)], check=True)
    print(f"Backup: {backup}")

    # Build new marker section
    marker_section = f"{BEGIN_MARKER} {new_hash} ###\n{new_content}\n{END_MARKER}"

    if has_markers:
        # Replace existing marker section
        lines = cfg_text.split("\n")
        new_lines: list[str] = []
        in_section = False
        for line in lines:
            if line.startswith(BEGIN_MARKER):
                in_section = True
                new_lines.append(marker_section)
                continue
            if line.startswith(END_MARKER):
                in_section = False
                continue
            if not in_section:
                new_lines.append(line)
        new_cfg = "\n".join(new_lines)
        action = "Updated"
    else:
        # No markers — insert after last 'set' line in preamble
        lines = cfg_text.split("\n")
        last_set = -1
        for i, line in enumerate(lines):
            if line.startswith("set "):
                last_set = i
        if last_set >= 0:
            new_lines = lines[: last_set + 1] + ["", marker_section] + lines[last_set + 1 :]
        else:
            new_lines = [marker_section, ""] + lines
        new_cfg = "\n".join(new_lines)
        action = "Inserted"

    # Write via temp file + sudo cp (EFI partition requires root)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".cfg", delete=False) as f:
        f.write(new_cfg)
        tmpfile = f.name
    subprocess.run(["sudo", "cp", tmpfile, str(GRUB_CFG)], check=True)
    Path(tmpfile).unlink()

    print(f"{action} dev entries in {GRUB_CFG}")
    print("Done. Default boot entry is 0 (LTS).")


if __name__ == "__main__":
    main()
