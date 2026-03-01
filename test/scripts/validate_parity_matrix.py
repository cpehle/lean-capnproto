#!/usr/bin/env python3
"""Validate Lean4 RPC parity matrix structure and deterministic test mapping."""

from __future__ import annotations

import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Iterable


ALLOWED_STATUS = {"covered", "partial", "blocked"}
REQUIRED_ENTRY_FIELDS = {
    "behaviorClass",
    "cxxReferences",
    "leanTests",
    "status",
    "notes",
}
LEAN_TEST_NAME_RE = re.compile(r"^Test\.[A-Za-z0-9_'.]+\.[A-Za-z0-9_']+$")


def _parse_test_declarations(tests_dir: Path, errors: list[str]) -> set[str]:
    attr_re = re.compile(r"^\s*@\[(test|test_ignore|test_should_error)\]\s*$")
    decl_re = re.compile(r"^\s*(?:private\s+)?(?:unsafe\s+)?(?:def|theorem)\s+([A-Za-z0-9_']+)\b")
    declarations: set[str] = set()

    for file_path in sorted(tests_dir.glob("*.lean")):
        module_name = f"Test.{file_path.stem}"
        lines = file_path.read_text(encoding="utf-8").splitlines()

        for idx, line in enumerate(lines):
            if not attr_re.match(line):
                continue

            saw_decl = False
            in_block_comment = False
            cursor = idx + 1
            while cursor < len(lines):
                candidate = lines[cursor].strip()
                if in_block_comment:
                    if "-/" in candidate:
                        in_block_comment = False
                    cursor += 1
                    continue

                if candidate.startswith("/-"):
                    if "-/" not in candidate:
                        in_block_comment = True
                    cursor += 1
                    continue

                if not candidate or candidate.startswith("--"):
                    cursor += 1
                    continue

                if candidate.startswith("@["):
                    cursor += 1
                    continue

                match = decl_re.match(lines[cursor])
                if match is None:
                    errors.append(
                        f"{file_path}:{cursor + 1}: expected def/theorem after test attribute at line {idx + 1}"
                    )
                else:
                    declarations.add(f"{module_name}.{match.group(1)}")
                saw_decl = True
                break

            if not saw_decl:
                errors.append(f"{file_path}:{idx + 1}: test attribute has no following declaration")

    return declarations


def _parse_parity_critical_tests(driver_path: Path, errors: list[str]) -> list[str]:
    content = driver_path.read_text(encoding="utf-8")
    block_match = re.search(
        r"private def parityCriticalTests\s*:[^=]+:=\s*#\[(.*?)\]",
        content,
        flags=re.DOTALL,
    )
    if block_match is None:
        errors.append(f"{driver_path}: could not find parityCriticalTests array declaration")
        return []

    short_names = re.findall(r"`([A-Za-z0-9_'.]+)", block_match.group(1))
    if not short_names:
        errors.append(f"{driver_path}: parityCriticalTests array is empty or unreadable")
        return []

    duplicates = sorted(name for name, count in Counter(short_names).items() if count > 1)
    if duplicates:
        errors.append(
            f"{driver_path}: parityCriticalTests contains duplicates: {', '.join(duplicates)}"
        )

    return short_names


def _resolve_short_names(
    short_names: Iterable[str], declarations: set[str], driver_path: Path, errors: list[str]
) -> list[str]:
    by_short: dict[str, list[str]] = defaultdict(list)
    for full_name in sorted(declarations):
        by_short[full_name.rsplit(".", 1)[-1]].append(full_name)

    resolved: list[str] = []
    for short_name in short_names:
        matches = by_short.get(short_name, [])
        if not matches:
            errors.append(
                f"{driver_path}: parityCriticalTests entry `{short_name}` does not resolve to any @[test] declaration in test/lean4/Test/*.lean"
            )
            continue
        if len(matches) > 1:
            errors.append(
                f"{driver_path}: parityCriticalTests entry `{short_name}` is ambiguous; matches: {', '.join(matches)}"
            )
            continue
        resolved.append(matches[0])

    return resolved


def _validate_string_list(
    value: object, path: str, field: str, errors: list[str], allow_empty: bool = False
) -> list[str]:
    if not isinstance(value, list):
        errors.append(f"{path}: `{field}` must be an array of strings")
        return []

    entries: list[str] = []
    for index, item in enumerate(value):
        if not isinstance(item, str) or not item.strip():
            errors.append(f"{path}: `{field}[{index}]` must be a non-empty string")
            continue
        entries.append(item.strip())

    if not allow_empty and not entries:
        errors.append(f"{path}: `{field}` must contain at least one entry")

    duplicates = sorted(name for name, count in Counter(entries).items() if count > 1)
    if duplicates:
        errors.append(f"{path}: `{field}` contains duplicates: {', '.join(duplicates)}")

    return entries


def _validate_matrix_entries(
    matrix: object, matrix_path: Path, errors: list[str]
) -> list[str]:
    if not isinstance(matrix, list):
        errors.append(f"{matrix_path}: top-level JSON value must be an array")
        return []
    if not matrix:
        errors.append(f"{matrix_path}: matrix is empty")
        return []

    seen_behavior_classes: set[str] = set()
    lean_tests: list[str] = []

    for idx, entry in enumerate(matrix, start=1):
        entry_path = f"{matrix_path}: entry {idx}"
        if not isinstance(entry, dict):
            errors.append(f"{entry_path}: must be an object")
            continue

        keys = set(entry.keys())
        missing = sorted(REQUIRED_ENTRY_FIELDS - keys)
        extra = sorted(keys - REQUIRED_ENTRY_FIELDS)
        if missing:
            errors.append(f"{entry_path}: missing required keys: {', '.join(missing)}")
        if extra:
            errors.append(f"{entry_path}: unexpected keys: {', '.join(extra)}")

        behavior_class = entry.get("behaviorClass")
        if not isinstance(behavior_class, str) or not behavior_class.strip():
            errors.append(f"{entry_path}: `behaviorClass` must be a non-empty string")
        else:
            behavior_class = behavior_class.strip()
            if behavior_class in seen_behavior_classes:
                errors.append(f"{entry_path}: duplicate `behaviorClass`: {behavior_class}")
            seen_behavior_classes.add(behavior_class)

        _validate_string_list(entry.get("cxxReferences"), entry_path, "cxxReferences", errors)

        status = entry.get("status")
        if not isinstance(status, str):
            errors.append(f"{entry_path}: `status` must be a string")
        elif status not in ALLOWED_STATUS:
            allowed = ", ".join(sorted(ALLOWED_STATUS))
            errors.append(
                f"{entry_path}: `status` must be one of [{allowed}], got `{status}`"
            )

        allow_empty_lean_tests = status == "blocked"
        entry_lean_tests = _validate_string_list(
            entry.get("leanTests"),
            entry_path,
            "leanTests",
            errors,
            allow_empty=allow_empty_lean_tests,
        )
        lean_tests.extend(entry_lean_tests)

        notes = entry.get("notes")
        if not isinstance(notes, str) or not notes.strip():
            errors.append(f"{entry_path}: `notes` must be a non-empty string")

    for lean_test in lean_tests:
        if LEAN_TEST_NAME_RE.match(lean_test):
            continue
        errors.append(
            f"{matrix_path}: invalid Lean test name `{lean_test}`; expected format `Test.<Module>.<testName>`"
        )

    duplicates = sorted(name for name, count in Counter(lean_tests).items() if count > 1)
    if duplicates:
        errors.append(
            f"{matrix_path}: duplicate Lean tests across behavior classes: {', '.join(duplicates)}"
        )

    return lean_tests


def main() -> int:
    repo_root = Path(__file__).resolve().parents[3]
    matrix_path = repo_root / "test" / "lean4" / "parity_matrix.json"
    driver_path = repo_root / "test" / "lean4" / "TestDriverRpc.lean"
    tests_dir = repo_root / "test" / "lean4" / "Test"
    errors: list[str] = []

    try:
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(f"Parity matrix validation failed:\n- missing file: {matrix_path}")
        return 1
    except json.JSONDecodeError as exc:
        print(f"Parity matrix validation failed:\n- {matrix_path}: invalid JSON: {exc}")
        return 1

    matrix_lean_tests = _validate_matrix_entries(matrix, matrix_path, errors)
    declarations = _parse_test_declarations(tests_dir, errors)
    parity_short_names = _parse_parity_critical_tests(driver_path, errors)
    parity_full_names = _resolve_short_names(parity_short_names, declarations, driver_path, errors)

    unknown_matrix_tests = sorted(set(matrix_lean_tests) - declarations)
    if unknown_matrix_tests:
        errors.append(
            f"{matrix_path}: matrix references Lean tests without @[test] declarations: {', '.join(unknown_matrix_tests)}"
        )

    matrix_test_set = set(matrix_lean_tests)
    parity_test_set = set(parity_full_names)

    missing_from_matrix = sorted(parity_test_set - matrix_test_set)
    if missing_from_matrix:
        errors.append(
            f"{matrix_path}: missing parity-critical Lean tests declared in {driver_path}: {', '.join(missing_from_matrix)}"
        )

    extra_in_matrix = sorted(matrix_test_set - parity_test_set)
    if extra_in_matrix:
        errors.append(
            f"{matrix_path}: matrix contains Lean tests not declared in parityCriticalTests: {', '.join(extra_in_matrix)}"
        )

    if errors:
        print("Parity matrix validation failed:")
        for err in errors:
            print(f"- {err}")
        print(
            "Fix by updating test declarations in test/lean4/TestDriverRpc.lean and/or test/lean4/parity_matrix.json."
        )
        return 1

    print("Parity matrix validation passed.")
    print(f"- Behavior classes: {len(matrix)}")
    print(f"- Lean tests in matrix: {len(matrix_test_set)}")
    print(f"- Parity-critical tests: {len(parity_test_set)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
