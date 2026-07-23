#!/usr/bin/env python3
"""Reject documentation drift from the shipped protocol and client ABI."""

from pathlib import Path
import re
import subprocess
import sys


def fail(message: str) -> None:
    print(f"documentation contract failure: {message}", file=sys.stderr)
    raise SystemExit(1)


if len(sys.argv) != 3:
    fail("expected <source-root> <nvecd-cli>")

source_root = Path(sys.argv[1])
cli_path = Path(sys.argv[2])
docs = sorted((source_root / "docs").glob("**/*.md"))
all_docs = "\n".join(path.read_text(encoding="utf-8") for path in docs)

for stale in (
    "libnvecdclient.a",
    "--log-level",
    "nvecd-test",
    "result->ids",
    "result->scores",
    "err->message",
    "VECSET item1 3 0.1 0.5 0.8",
    "--target uninstall",
):
    if stale in all_docs:
        fail(f"stale text remains: {stale}")

for pattern, description in (
    (r"DUMP\s+(?:SAVE|LOAD)\s+/backup(?:\s|$)", "snapshot directory used as a file"),
    (
        r'nvecdclient_event\(\s*client,\s*"[^"]+",\s*"[^"]+",\s*\d+\s*\)',
        "four-argument C EVENT call",
    ),
    (
        r'client\.Event\(\s*"[^"]+",\s*"[^"]+",\s*\d+\s*\)',
        "three-argument C++ EVENT call",
    ),
    (r"if\s*\(\s*auto\s+err\s*=\s*client\.", "inverted Expected example"),
):
    if re.search(pattern, all_docs):
        fail(description)

for language in ("en", "ja"):
    client_doc = (source_root / "docs" / language / "libnvecdclient.md").read_text(
        encoding="utf-8"
    )
    for required in (
        "nvecd::client",
        "result.error().message()",
        "total_commands_processed",
        "ctypes.c_uint16",
        "ctypes.c_size_t",
        "result->results[i].id",
    ):
        if required not in client_doc:
            fail(f"{language} client guide is missing: {required}")

    installation_doc = (
        source_root / "docs" / language / "installation.md"
    ).read_text(encoding="utf-8")
    for required in ("--password-file", "--password-env"):
        if required not in installation_doc:
            fail(f"{language} installation guide is missing: {required}")

help_result = subprocess.run(
    [str(cli_path), "--help"],
    check=False,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
)
if help_result.returncode != 0:
    fail(f"nvecd-cli --help returned {help_result.returncode}")
for required in ("--password-file FILE", "--password-env NAME", "--wait-ready"):
    if required not in help_result.stdout:
        fail(f"CLI help is missing: {required}")

print(f"documentation contracts verified across {len(docs)} files")
