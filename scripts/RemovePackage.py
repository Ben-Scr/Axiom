#!/usr/bin/env python3
"""
Axiom package remover — inverse of `scripts/NewPackage.py`.

Deletes a package directory, removes it from a project's `axiom-project.json`
allow-list (when `--project` is given), and re-runs premake so the project
disappears from the IDE solution on next reload.

Usage examples
--------------
    # Engine package:
    python scripts/RemovePackage.py Axiom.Foo

    # Project-local package (also removes from axiom-project.json):
    python scripts/RemovePackage.py MyGame.Loot --project "C:/path/to/MyProject"

    # Skip premake regen (CI / batch removal):
    python scripts/RemovePackage.py Axiom.Foo --no-premake

    # Skip the directory delete (only remove from allow-list):
    python scripts/RemovePackage.py MyGame.Loot --project "C:/path/to/MyProject" --keep-files

By default the script prompts before deleting non-empty directories. Pass
`--force` to skip the prompt (useful for CI).
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent
PREMAKE_EXE = REPO_ROOT / "vendor" / "bin" / "premake5.exe"

PACKAGE_NAME_PATTERN = re.compile(r"^[A-Z][A-Za-z0-9]+(\.[A-Z][A-Za-z0-9]+)+$")


def remove_from_project_allow_list(project_root: Path, name: str) -> bool:
    """Remove `name` from <project>/axiom-project.json's `packages` array.

    Returns True if the file was modified, False if the entry wasn't present
    or the file couldn't be parsed.
    """
    project_file = project_root / "axiom-project.json"
    if not project_file.is_file():
        print(
            f"[RemovePackage] WARNING: --project given but {project_file} not found. "
            "Skipping allow-list update."
        )
        return False

    try:
        data = json.loads(project_file.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(
            f"[RemovePackage] WARNING: {project_file} is not parseable JSON ({exc}). "
            "Skipping allow-list update — remove the package name manually."
        )
        return False

    packages = data.get("packages")
    if not isinstance(packages, list) or name not in packages:
        print(f"[RemovePackage] '{name}' not in {project_file.name} packages list — nothing to remove.")
        return False

    data["packages"] = [p for p in packages if p != name]
    project_file.write_text(
        json.dumps(data, indent=4, sort_keys=False) + "\n", encoding="utf-8"
    )
    print(f"  removed '{name}' from {project_file.relative_to(project_root)}")
    return True


def delete_package_dir(pkg_dir: Path, force: bool) -> bool:
    """Recursively delete the package directory after a confirmation prompt.

    Returns True if the directory was removed (or never existed); False if the
    user declined.
    """
    if not pkg_dir.exists():
        print(f"[RemovePackage] {pkg_dir} doesn't exist — nothing to delete.")
        return True

    if not force:
        # Find a portable size hint so the user knows what they're nuking.
        file_count = sum(1 for _ in pkg_dir.rglob("*") if _.is_file())
        print(f"[RemovePackage] About to delete {pkg_dir} ({file_count} files).")
        try:
            response = input("Continue? [y/N] ").strip().lower()
        except EOFError:
            response = ""
        if response not in {"y", "yes"}:
            print("[RemovePackage] Aborted (no files removed).")
            return False

    shutil.rmtree(pkg_dir)
    print(f"  removed {pkg_dir}")
    return True


def run_premake(project_path: Path | None) -> None:
    if not PREMAKE_EXE.is_file():
        print(
            f"[RemovePackage] premake5 not found at {PREMAKE_EXE} — skipping regen. "
            "Run scripts/Setup.py first or pass --no-premake."
        )
        return
    cmd = [str(PREMAKE_EXE), "vs2022"]
    if project_path is not None:
        cmd.append(f"--axiom-project={project_path}")
    print(f"[RemovePackage] regenerating projects: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=str(REPO_ROOT))
    if result.returncode != 0:
        raise SystemExit(f"[RemovePackage] premake regen failed (exit code {result.returncode}).")


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Remove an Axiom package (inverse of scripts/NewPackage.py).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("name", help="Package name in PascalCase.PascalCase form.")
    parser.add_argument(
        "--project",
        default=None,
        help="Absolute path to an Axiom project. If set, the package is looked up under "
        "<project>/Packages/<Name>/ AND removed from <project>/axiom-project.json. "
        "Otherwise, looked up under <repo>/packages/<Name>/.",
    )
    parser.add_argument(
        "--keep-files",
        action="store_true",
        help="Don't delete the package directory — only update the allow-list. "
        "Useful for temporarily disabling a package without losing its source.",
    )
    parser.add_argument(
        "--no-premake",
        action="store_true",
        help="Skip running premake5 after removal.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Skip the confirmation prompt before deleting the package directory.",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    name: str = args.name.strip()
    if not PACKAGE_NAME_PATTERN.match(name):
        raise SystemExit(
            f"[RemovePackage] '{name}' is not a valid Axiom package name. "
            "Expected PascalCase.PascalCase[.PascalCase…]."
        )

    project_root: Path | None = None
    if args.project:
        project_root = Path(args.project).expanduser().resolve()
        if not project_root.is_dir():
            raise SystemExit(f"[RemovePackage] --project path is not a directory: {project_root}")
        pkg_dir = project_root / "Packages" / name
    else:
        pkg_dir = REPO_ROOT / "packages" / name

    print(f"[RemovePackage] removing '{name}'")

    # 1. Allow-list (project-local only). Done first so the file change is
    #    visible even if the directory delete is declined.
    if project_root is not None:
        remove_from_project_allow_list(project_root, name)

    # 2. Files. Skipped when --keep-files is set.
    if args.keep_files:
        print(f"[RemovePackage] --keep-files: leaving {pkg_dir} on disk.")
    else:
        if not delete_package_dir(pkg_dir, force=args.force):
            return 1  # user aborted

    # 3. Premake regen.
    if not args.no_premake:
        run_premake(project_root)

    print()
    print("[RemovePackage] done. Restart Visual Studio so the removed project disappears from Axiom.sln.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
