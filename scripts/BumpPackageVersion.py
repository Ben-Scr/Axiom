#!/usr/bin/env python3
"""
Bump the `version` field in an Axiom package's `axiom-package.lua` manifest.

Usage examples
--------------
    # Bump patch (0.1.0 -> 0.1.1):
    python scripts/BumpPackageVersion.py Axiom.Tilemap2D --patch

    # Bump minor (0.1.0 -> 0.2.0; resets patch to 0):
    python scripts/BumpPackageVersion.py Axiom.Tilemap2D --minor

    # Bump major (0.1.0 -> 1.0.0; resets minor and patch to 0):
    python scripts/BumpPackageVersion.py Axiom.Tilemap2D --major

    # Set an explicit version (any semver-shaped string is accepted):
    python scripts/BumpPackageVersion.py Axiom.Tilemap2D --set 2.1.3

    # Project-local package (looked up under <project>/Packages/<Name>/):
    python scripts/BumpPackageVersion.py MyGame.Loot --patch \\
        --project "C:/path/to/MyProject"

    # Dry-run — show what the change would be without writing:
    python scripts/BumpPackageVersion.py Axiom.Tilemap2D --patch --dry-run

The script does a regex-based rewrite — it doesn't parse the full Lua
manifest. The `version` field MUST be on its own line in the form
`version = "X.Y.Z"` (with any amount of whitespace) for the bump to land.
That matches every manifest the scaffolder generates.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent

PACKAGE_NAME_PATTERN = re.compile(r"^[A-Z][A-Za-z0-9]+(\.[A-Z][A-Za-z0-9]+)+$")
# Matches: `version` `=` `"X.Y.Z"` with arbitrary whitespace + an optional
# trailing comma. Captures the version string so we can rewrite just that
# slice without touching anything else on the line.
VERSION_FIELD_PATTERN = re.compile(
    r'^(?P<prefix>\s*version\s*=\s*")(?P<version>[^"]+)(?P<suffix>".*)$',
    re.MULTILINE,
)
SEMVER_PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def parse_semver(version: str) -> tuple[int, int, int]:
    match = SEMVER_PATTERN.match(version.strip())
    if not match:
        raise SystemExit(
            f"[BumpPackageVersion] Cannot bump version '{version}' — not in MAJOR.MINOR.PATCH form. "
            "Use --set X.Y.Z to write an explicit version instead."
        )
    return int(match.group(1)), int(match.group(2)), int(match.group(3))


def bump(version: str, kind: str) -> str:
    major, minor, patch = parse_semver(version)
    if kind == "major":
        return f"{major + 1}.0.0"
    if kind == "minor":
        return f"{major}.{minor + 1}.0"
    if kind == "patch":
        return f"{major}.{minor}.{patch + 1}"
    raise ValueError(f"Unknown bump kind: {kind}")


def resolve_manifest_path(name: str, project_path: Path | None) -> Path:
    if project_path is not None:
        return project_path / "Packages" / name / "axiom-package.lua"
    return REPO_ROOT / "packages" / name / "axiom-package.lua"


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Bump or set the version of an Axiom package's axiom-package.lua manifest.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("name", help="Package name in PascalCase.PascalCase form.")
    bump_kind = parser.add_mutually_exclusive_group(required=True)
    bump_kind.add_argument("--major", action="store_true", help="Bump the major version (X+1.0.0).")
    bump_kind.add_argument("--minor", action="store_true", help="Bump the minor version (X.Y+1.0).")
    bump_kind.add_argument("--patch", action="store_true", help="Bump the patch version (X.Y.Z+1).")
    bump_kind.add_argument("--set", dest="explicit", metavar="X.Y.Z",
                           help="Set the version to an explicit string. Any semver-shaped value is accepted.")
    parser.add_argument(
        "--project",
        default=None,
        help="Absolute path to an Axiom project. Looks up the package under "
        "<project>/Packages/<Name>/. Otherwise looks under <repo>/packages/<Name>/.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print what would change without writing the file.",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    name = args.name.strip()
    if not PACKAGE_NAME_PATTERN.match(name):
        raise SystemExit(
            f"[BumpPackageVersion] '{name}' is not a valid Axiom package name."
        )

    project_root: Path | None = None
    if args.project:
        project_root = Path(args.project).expanduser().resolve()
        if not project_root.is_dir():
            raise SystemExit(f"[BumpPackageVersion] --project path is not a directory: {project_root}")

    manifest_path = resolve_manifest_path(name, project_root)
    if not manifest_path.is_file():
        raise SystemExit(f"[BumpPackageVersion] No manifest at {manifest_path}.")

    text = manifest_path.read_text(encoding="utf-8")
    match = VERSION_FIELD_PATTERN.search(text)
    if not match:
        raise SystemExit(
            f"[BumpPackageVersion] Could not locate `version = \"...\"` line in {manifest_path}. "
            "The field must be on its own line for the regex bump to work; edit it manually."
        )

    old_version = match.group("version")
    if args.explicit:
        new_version = args.explicit.strip()
        if not new_version:
            raise SystemExit("[BumpPackageVersion] --set requires a non-empty version string.")
    elif args.major:
        new_version = bump(old_version, "major")
    elif args.minor:
        new_version = bump(old_version, "minor")
    else:  # --patch
        new_version = bump(old_version, "patch")

    if new_version == old_version:
        print(f"[BumpPackageVersion] {name} is already at {old_version} — nothing to do.")
        return 0

    new_text = text[:match.start("version")] + new_version + text[match.end("version"):]

    rel = manifest_path.relative_to(REPO_ROOT) if manifest_path.is_relative_to(REPO_ROOT) else manifest_path
    if args.dry_run:
        print(f"[BumpPackageVersion] (dry-run) {rel}: {old_version} -> {new_version}")
        return 0

    manifest_path.write_text(new_text, encoding="utf-8", newline="\n")
    print(f"[BumpPackageVersion] {rel}: {old_version} -> {new_version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
