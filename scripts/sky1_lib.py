"""Shared library for Sky1 kernel build scripts.

Board detection, kernel version parsing, EFI name derivation,
and git helpers used by update-dev-boot.py, kernel-track-status.py,
and build-install.py.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

# Board configurations: DT compatible string → DTS basename / DTB EFI prefix
# Also maps DMI board_name (from ACPI/SMBIOS) → DT compatible for fallback
BOARD_DTS: dict[str, str] = {
    "radxa,orion-o6": "sky1-orion-o6",
    "radxa,orion-o6n": "sky1-orion-o6n",
    "xunlong,orangepi-6-plus": "sky1-orangepi-6-plus",
}

BOARD_DTB_PREFIX: dict[str, str] = {
    "radxa,orion-o6": "SKY1-ORION-O6",
    "radxa,orion-o6n": "SKY1-ORION-O6N",
    "xunlong,orangepi-6-plus": "SKY1-ORANGEPI-6-PLUS",
}

DMI_TO_COMPAT: dict[str, str] = {
    "Radxa Orion O6": "radxa,orion-o6",
    "Radxa Orion O6N": "radxa,orion-o6n",
    "OrangePi 6 Plus": "xunlong,orangepi-6-plus",
}

# Track configuration
BRANCHES = ["main", "latest", "rc", "next"]

TRACK_LABELS: dict[str, str] = {
    "main": "LTS",
    "latest": "",
    "rc": "RC",
    "next": "next",
}


@dataclass
class Board:
    compat: str
    dts_base: str
    dtb_prefix: str


@dataclass
class KernelVersion:
    major: int
    minor: int
    sublevel: int
    extra: str

    @property
    def major_minor(self) -> str:
        return f"{self.major}.{self.minor}"

    @property
    def full(self) -> str:
        # Match upstream tag format: 6.19-rc7 (no .0 sublevel for pre-release)
        # vs 6.18.7 (include sublevel for stable)
        if self.sublevel == 0 and self.extra:
            return f"{self.major}.{self.minor}{self.extra}"
        return f"{self.major}.{self.minor}.{self.sublevel}{self.extra}"


@dataclass
class EFINames:
    image: str
    dtb: str


def detect_board() -> Board:
    """Detect current board from DT compatible string or DMI board_name."""
    compat_path = Path("/sys/firmware/devicetree/base/compatible")
    dmi_path = Path("/sys/class/dmi/id/board_name")

    compat = None
    if compat_path.exists():
        compat = compat_path.read_bytes().split(b"\x00")[0].decode()
    elif dmi_path.exists():
        board_name = dmi_path.read_text().strip()
        compat = DMI_TO_COMPAT.get(board_name)
        if not compat:
            raise RuntimeError(
                f"Unknown DMI board '{board_name}'. "
                f"Known: {', '.join(DMI_TO_COMPAT.keys())}"
            )
    else:
        raise RuntimeError("Cannot detect board: no DT or DMI available")

    if compat not in BOARD_DTS:
        known = ", ".join(BOARD_DTS.keys())
        raise RuntimeError(f"Unknown board '{compat}'. Known boards: {known}")

    return Board(
        compat=compat,
        dts_base=BOARD_DTS[compat],
        dtb_prefix=BOARD_DTB_PREFIX[compat],
    )


def get_kernel_version(ref: str) -> KernelVersion:
    """Get kernel version from a git ref's Makefile without checkout."""
    result = git("show", f"{ref}:Makefile", check=False)
    if result.returncode != 0:
        raise RuntimeError(f"Cannot read Makefile from ref '{ref}'")

    version = patchlevel = sublevel = ""
    extraversion = ""
    for line in result.stdout.split("\n")[:10]:
        if m := re.match(r"^VERSION\s*=\s*(\d+)", line):
            version = m.group(1)
        elif m := re.match(r"^PATCHLEVEL\s*=\s*(\d+)", line):
            patchlevel = m.group(1)
        elif m := re.match(r"^SUBLEVEL\s*=\s*(\d+)", line):
            sublevel = m.group(1)
        elif m := re.match(r"^EXTRAVERSION\s*=\s*(.*)", line):
            extraversion = m.group(1).strip()

    return KernelVersion(
        major=int(version),
        minor=int(patchlevel),
        sublevel=int(sublevel) if sublevel else 0,
        extra=extraversion,
    )


def get_efi_names(branch: str, major_minor: str, dtb_prefix: str) -> EFINames:
    """Derive EFI filenames from branch name and kernel version."""
    match branch:
        case "main" | "latest":
            return EFINames(
                image=f"IMAGE-{major_minor}-sky1",
                dtb=f"{dtb_prefix}-{major_minor}-sky1.DTB",
            )
        case "rc":
            return EFINames(
                image=f"IMAGE-{major_minor}-sky1-rc",
                dtb=f"{dtb_prefix}-{major_minor}-sky1-rc.DTB",
            )
        case "next":
            return EFINames(
                image="IMAGE-next-sky1",
                dtb=f"{dtb_prefix}-next-sky1.DTB",
            )
        case _:
            raise ValueError(f"Unknown branch '{branch}'")


def git(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    """Run a git command and return the result."""
    return subprocess.run(
        ["git", *args],
        capture_output=True,
        text=True,
        check=check,
    )


def branch_exists(branch: str) -> bool:
    """Check if a git branch exists locally."""
    result = git("rev-parse", "--verify", branch, check=False)
    return result.returncode == 0
