#!/usr/bin/env python3
"""Reject documentation drift from the shipped protocol, config and client ABI.

The same promise -- a wire field name, a default, a status word, an endpoint --
is written down in several places: `docs/en/*.md`, `docs/ja/*.md`,
`src/config/config-schema.json`, public headers and the CLI help. Nothing keeps
those copies equal except this file, so each check below fixes one
correspondence rather than spot-checking one sentence:

* the configuration reference and the example config are generated from the
  schema and must be current;
* every schema key is parsed, introspectable and actually read by the server;
* every field name, status word and JSON key the docs promise exists verbatim
  in the source that produces it;
* the English and Japanese guides state the same set of facts;
* every error code is reachable from a `MakeError` call site.
"""

from pathlib import Path
import json
import re
import subprocess
import sys

# Schema keys that no code outside src/config reads. A key here is parsed,
# validated and echoed by CONFIG SHOW while configuring nothing, so it lies
# about being configurable. Nothing may be added to this tuple.
UNREAD_SCHEMA_KEYS: tuple[str, ...] = ()

# Error codes that no MakeError call site constructs. They are unreachable, and
# an unreachable code is a promise of a diagnostic the server cannot emit. This
# tuple may only shrink -- an entry leaves it by being wired up or removed.
ERROR_CODES_WITHOUT_CONSTRUCTOR = (
    "kCacheDisabled",
    "kCacheMiss",
    "kClientInvalidResponse",
    "kCommandInvalidMode",
    "kCommandInvalidToken",
    "kCommandMissingArgument",
    "kCommandTooLong",
    "kCommandUnexpectedToken",
    "kConfigJsonError",
    "kConfigMissingRequired",
    "kConfigSchemaError",
    "kEventCtxBufferFull",
    "kEventCtxNotFound",
    "kNetworkAcceptFailed",
    "kNetworkConnectionClosed",
    "kNetworkConnectionRefused",
    "kNetworkIPNotAllowed",
    "kNetworkInvalidRequest",
    "kNetworkProtocolError",
    "kNetworkRateLimited",
    "kNetworkReceiveFailed",
    "kNetworkSendFailed",
    "kNetworkServerNotStarted",
    "kOutOfRange",
    "kSimilarityComputeError",
    "kSimilarityInvalidMode",
    "kSimilarityNoResults",
    "kStorageCRCMismatch",
    "kStorageCompressionFailed",
    "kStorageDecompressionFailed",
    "kStorageDocIdExhausted",
    "kStorageFileNotFound",
    "kStorageReadError",
    "kStorageSnapshotBuildFailed",
    "kStorageVersionMismatch",
    "kSuccess",
    "kTimeout",
    "kUnknown",
    "kVectorStoreError",
    "kVectorStoreFull",
)

# Documentation pages whose vocabulary the server must produce verbatim.
WIRE_DOCS = ("protocol.md", "http-api.md", "persistence.md")

# A token that names something the reader can type or receive: an identifier,
# a dotted path, an endpoint, an HTTP verb plus path, or a status code. Prose
# fragments and signature spellings are excluded so translation style does not
# count as a difference in fact.
IDENTIFIER = re.compile(
    r"^(?:[A-Za-z_][A-Za-z0-9_.]*(?:\(\))?"
    r"|/[A-Za-z0-9/._-]+"
    r"|[A-Z]+ /[A-Za-z0-9/._-]+"
    r"|[1-5][0-9]{2})$"
)

# A response field name: snake_case with at least two words, which makes it
# specific enough that finding it in the source means something.
FIELD_NAME = re.compile(r"^[a-z][a-z0-9]*(?:_[a-z0-9]+)+$")

# A protocol status or keyword token.
STATUS_WORD = re.compile(r"^[A-Z][A-Z0-9_]+$")

FENCED_BLOCK = re.compile(r"```(\w*)\n(.*?)```", re.DOTALL)

# A success line in a documented protocol transcript, with or without the arrow
# the examples use to mark the server's side.
RESPONSE_LINE = re.compile(r"^(?:→\s*)?\+?OK(?:\s+(?P<status>\S+))?")

# The word that follows OK on such a line. Matching any case rather than only
# ALL-CAPS matters: an invented "OK Snapshot saved: ..." reads like a real
# response precisely because it is not shouted.
RESPONSE_STATUS = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")

# A C++ method as the client guide spells it, e.g.
# "Expected<SaveResult, Error> Save(filepath)" or "bool IsConnected()".
CPP_SIGNATURE = re.compile(
    r"^(?:Expected<\s*(?P<returns>[\w:<>, ]+?)\s*,\s*Error\s*>|void|bool)"
    r"\s+(?P<name>\w+)\("
)

# A C entry point as the guide spells it, e.g. "nvecdclient_auth()".
C_FUNCTION = re.compile(r"^nvecdclient_\w+\(\)$")

failures: list[str] = []


def fail(message: str) -> None:
    """Record a broken contract; every check runs before the test exits."""
    failures.append(message)


def strip_code(text: str) -> str:
    """Drop fenced blocks so prose checks do not read sample code."""
    return FENCED_BLOCK.sub("", text)


def backticked(text: str) -> set[str]:
    """Return every inline-code token in a document."""
    return set(re.findall(r"`([^`\n]{1,60})`", strip_code(text)))


def headings(text: str) -> list[int]:
    """Return the nesting level of every heading, in document order."""
    return [len(m.group(1)) for m in re.finditer(r"^(#{1,6}) ", strip_code(text), re.M)]


def schema_leaves(schema: dict) -> dict:
    """Return every leaf key of the configuration schema by dotted path."""
    leaves = {}

    def visit(node: dict, dotted: str):
        properties = node.get("properties")
        if properties is None:
            leaves[dotted] = node
            return
        for name, child in properties.items():
            visit(child, f"{dotted}.{name}" if dotted else name)

    visit(schema, "")
    return leaves


def check_generated_docs(source_root: Path) -> None:
    """The configuration reference and example config are current."""
    result = subprocess.run(
        [
            sys.executable,
            str(source_root / "support" / "generate_config_docs.py"),
            "--check",
            "--source-root",
            str(source_root),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        fail(f"generated configuration docs are stale:\n{result.stdout.strip()}")


def check_schema_wiring(source_root: Path) -> None:
    """Every schema key is parsed, introspectable and read by the server."""
    config_dir = source_root / "src" / "config"
    schema = json.loads((config_dir / "config-schema.json").read_text(encoding="utf-8"))
    leaves = schema_leaves(schema)

    manager = (config_dir / "runtime_variable_manager.cpp").read_text(encoding="utf-8")
    declared = set(re.findall(r'\{"([\w.]+)",\s*(?:true|false)\}', manager))
    resolved = set(re.findall(r'variable_name == "([\w.]+)"', manager))
    resolved |= set(re.findall(r'runtime_values_\["([\w.]+)"\]', manager))

    for name in sorted(set(leaves) - declared):
        fail(f"schema key is not declared in kVariableMutability: {name}")
    for name in sorted(declared - set(leaves)):
        fail(f"kVariableMutability declares a key the schema does not define: {name}")
    for name in sorted(declared - resolved):
        fail(f"SHOW VARIABLES cannot resolve a value for: {name}")

    parser = (config_dir / "config.cpp").read_text(encoding="utf-8")
    display = (config_dir / "config_help.cpp").read_text(encoding="utf-8")
    for dotted in sorted(leaves):
        key = dotted.rsplit(".", 1)[-1]
        if f'"{key}"' not in parser:
            fail(f"schema key has no parse site in config.cpp: {dotted}")
        if f'"{key}"' not in display:
            fail(f"schema key is absent from the CONFIG SHOW output: {dotted}")

    # A key the server never reads configures nothing, however carefully it is
    # parsed and documented. The struct field is named after the schema key
    # except where the parser converts units.
    field_names = {"cache.max_memory_mb": "max_memory_bytes"}
    consumers = "\n".join(
        path.read_text(encoding="utf-8", errors="ignore")
        for path in sorted((source_root / "src").rglob("*"))
        if path.is_file()
        and path.suffix in (".cpp", ".h", ".c")
        and "config" not in path.parts[-2:]
    )
    for dotted in sorted(leaves):
        if dotted in UNREAD_SCHEMA_KEYS:
            continue
        field = field_names.get(dotted, dotted.rsplit(".", 1)[-1])
        if not re.search(r"(?:\.|->)" + re.escape(field) + r"\b", consumers):
            fail(f"no code outside src/config reads the schema key: {dotted}")
    for dotted in UNREAD_SCHEMA_KEYS:
        if dotted not in leaves:
            fail(f"UNREAD_SCHEMA_KEYS names a key the schema dropped: {dotted}")

    # An integer key that accepts 0 almost always gives 0 a second meaning --
    # "disabled", "unlimited", "auto" -- and the value is useless, or actively
    # misleading, until the schema says which. `wal.sync_interval_ms` is the
    # cautionary case: 0 read as "no periodic fsync" but means "fsync always".
    # Continuous quantities are exempt: for a weight or a threshold, 0 is just
    # the bottom of the range.
    for dotted, node in sorted(leaves.items()):
        if node.get("type") != "integer" or node.get("minimum") != 0:
            continue
        for field in ("description", "description_ja"):
            if "0 =" not in node.get(field, ""):
                fail(f"schema key accepts 0 without saying what it means: {dotted}")

    check_documented_yaml_keys(source_root, schema, leaves)


def check_documented_yaml_keys(source_root: Path, schema: dict, leaves: dict) -> None:
    """Every key in a documented config sample is a key the schema defines."""
    sections = {dotted.rsplit(".", 1)[-1] for dotted, _ in walk_schema_sections(schema)}
    known = sections | {dotted.rsplit(".", 1)[-1] for dotted in leaves}
    top_level = "|".join(schema["properties"])

    pages = sorted((source_root / "docs").rglob("*.md"))
    pages += [source_root / "README.md", source_root / "README_ja.md"]
    for page in pages:
        text = page.read_text(encoding="utf-8")
        for language, block in FENCED_BLOCK.findall(text):
            if language != "yaml":
                continue
            lines = block.split("\n")
            # Only inspect blocks that are nvecd configuration rather than, say,
            # a Kubernetes manifest.
            if not any(re.match(rf"^({top_level}):\s*$", line) for line in lines):
                continue
            for line in lines:
                match = re.match(r"^\s*([a-z_][a-z0-9_]*):", line)
                if match and match.group(1) not in known:
                    fail(
                        f"{page.name} documents a config key the schema does not "
                        f"define: {match.group(1)}"
                    )


def walk_schema_sections(schema: dict):
    """Yield (dotted path, node) for every node that owns child properties."""

    def visit(node: dict, dotted: str):
        properties = node.get("properties", {})
        if dotted:
            yield dotted, node
        for name, child in properties.items():
            if "properties" in child:
                yield from visit(child, f"{dotted}.{name}" if dotted else name)

    yield from visit(schema, "")


def check_wire_vocabulary(source_root: Path) -> None:
    """Every documented field, status word and JSON key exists in the source."""
    sources = "\n".join(
        path.read_text(encoding="utf-8", errors="ignore")
        for path in sorted((source_root / "src").rglob("*"))
        if path.is_file() and path.suffix in (".cpp", ".h", ".c")
    )

    def produced(token: str) -> bool:
        return f'"{token}"' in sources or token in sources

    for language in ("en", "ja"):
        for name in WIRE_DOCS:
            path = source_root / "docs" / language / name
            text = path.read_text(encoding="utf-8")

            for token in sorted(backticked(text)):
                if not (FIELD_NAME.match(token) or STATUS_WORD.match(token)):
                    continue
                if not produced(token):
                    fail(
                        f"{language}/{name} documents a token no source produces: "
                        f"{token}"
                    )

            keys = set()
            for _language, block in FENCED_BLOCK.findall(text):
                keys |= set(re.findall(r'"([a-z][a-z0-9_]*)"\s*:', block))
            for key in sorted(keys):
                if not produced(key):
                    fail(
                        f"{language}/{name} documents a response field no source "
                        f"produces: {key}"
                    )

            # A transcript in a fenced block is the most literal promise the
            # docs make, and the easiest to invent. Every success line must
            # name a status the dispatcher can actually format.
            for _language, block in FENCED_BLOCK.findall(text):
                for line in block.split("\n"):
                    match = RESPONSE_LINE.match(line.strip())
                    if match is None:
                        continue
                    status = match.group("status")
                    # A bare word only. Placeholders such as "[data]" and
                    # "<path>" stand for a value rather than naming a status.
                    if status is None or not RESPONSE_STATUS.match(status):
                        continue
                    if f'"OK {status}' in sources:
                        continue
                    if f'FormatOK("{status}")' in sources:
                        continue
                    fail(
                        f"{language}/{name} shows a response the server cannot "
                        f"send: {line.strip()[:60]}"
                    )


def check_client_api_signatures(source_root: Path) -> None:
    """Signatures in the client guide match the shipped public headers.

    A public header doc comment outranks the guide, so when they disagree the
    guide is what moves. Nothing detected the disagreement until this check:
    `Save()` changed its return type in the header while both guides went on
    promising the old one.
    """
    client_dir = source_root / "src" / "client"
    cpp_header = (client_dir / "nvecdclient.h").read_text(encoding="utf-8")
    c_header = (client_dir / "nvecdclient_c.h").read_text(encoding="utf-8")

    for language in ("en", "ja"):
        guide = (
            source_root / "docs" / language / "client-library.md"
        ).read_text(encoding="utf-8")
        tokens = re.findall(r"`([^`\n]{1,90})`", guide)

        for token in tokens:
            match = CPP_SIGNATURE.match(token)
            if match is None:
                continue
            returns, name = match.group("returns"), match.group("name")
            if not re.search(rf"\b{re.escape(name)}\s*\(", cpp_header):
                fail(
                    f"{language} client guide documents a method that "
                    f"nvecdclient.h does not declare: {token}"
                )
                continue
            if returns is None:
                continue
            declared = re.search(
                r"Expected<\s*([\w:<>, ]+?)\s*,\s*[\w:]*Error\s*>\s+"
                rf"{re.escape(name)}\s*\(",
                cpp_header,
            )
            if declared is None:
                fail(
                    f"{language} client guide gives {name}() an Expected return "
                    f"that nvecdclient.h does not declare: {token}"
                )
            elif declared.group(1).split("::")[-1] != returns.split("::")[-1]:
                fail(
                    f"{language} client guide says {name}() returns "
                    f"'{returns}' but nvecdclient.h declares "
                    f"'{declared.group(1)}'"
                )

        for token in sorted({t for t in tokens if C_FUNCTION.match(t)}):
            name = token[:-2]
            if name.endswith("_"):
                continue
            if not re.search(rf"\b{re.escape(name)}\s*\(", c_header):
                fail(
                    f"{language} client guide documents a C function that "
                    f"nvecdclient_c.h does not declare: {token}"
                )


def check_language_parity(source_root: Path) -> None:
    """The English and Japanese guides state the same set of facts."""
    english_dir = source_root / "docs" / "en"
    for path in sorted(english_dir.glob("*.md")):
        counterpart = source_root / "docs" / "ja" / path.name
        if not counterpart.exists():
            fail(f"docs/ja is missing a counterpart for {path.name}")
            continue
        english = path.read_text(encoding="utf-8")
        japanese = counterpart.read_text(encoding="utf-8")

        if headings(english) != headings(japanese):
            fail(
                f"{path.name}: heading structure differs between en and ja "
                f"({len(headings(english))} vs {len(headings(japanese))} headings)"
            )

        english_tokens = {t for t in backticked(english) if IDENTIFIER.match(t)}
        japanese_tokens = {t for t in backticked(japanese) if IDENTIFIER.match(t)}
        for token in sorted(english_tokens - japanese_tokens):
            fail(f"{path.name}: documented in en but not in ja: {token}")
        for token in sorted(japanese_tokens - english_tokens):
            fail(f"{path.name}: documented in ja but not in en: {token}")


def make_error_arguments(sources: str) -> list[str]:
    """Argument text of every MakeError call, with nested parentheses balanced.

    The code is not always the first token: a call site may select between two
    of them with a conditional operator. Reading only the first argument would
    report a code that is constructed as unreachable, so the whole argument
    list is returned and searched.
    """
    arguments = []
    for call in re.finditer(r"\bMakeError\(", sources):
        depth = 1
        index = call.end()
        while index < len(sources) and depth > 0:
            if sources[index] == "(":
                depth += 1
            elif sources[index] == ")":
                depth -= 1
            index += 1
        arguments.append(sources[call.end() : index - 1])
    return arguments


def check_error_code_reachability(source_root: Path) -> None:
    """Every error code is constructed by at least one MakeError call site."""
    header = (source_root / "src" / "utils" / "error.h").read_text(encoding="utf-8")
    block = re.search(
        r"enum class ErrorCode : std::uint16_t \{(.*?)\n\};", header, re.DOTALL
    )
    if block is None:
        fail("cannot locate the ErrorCode enumeration in src/utils/error.h")
        return
    enumerators = [m.group(1) for m in re.finditer(r"^\s*(k\w+)\s*=", block.group(1), re.M)]

    sources = "\n".join(
        path.read_text(encoding="utf-8", errors="ignore")
        for path in sorted((source_root / "src").rglob("*"))
        if path.is_file() and path.suffix in (".cpp", ".h", ".c") and path.name != "error.h"
    )
    constructed = {
        name
        for arguments in make_error_arguments(sources)
        for name in re.findall(r"ErrorCode::(k\w+)", arguments)
    }

    recorded = set(ERROR_CODES_WITHOUT_CONSTRUCTOR)
    for name in sorted(set(enumerators) - constructed - recorded):
        fail(f"error code has no MakeError call site: {name}")
    for name in sorted(recorded - set(enumerators)):
        fail(f"ERROR_CODES_WITHOUT_CONSTRUCTOR names a code that no longer exists: {name}")
    # The tuple only shrinks, so an entry that has since been wired up has to
    # leave it. Without this the tuple silently accumulates codes that are in
    # fact reachable, and each one is a hole: should the code later lose its
    # last call site, nothing here would say so.
    for name in sorted(recorded & constructed):
        fail(f"ERROR_CODES_WITHOUT_CONSTRUCTOR names a code that is constructed: {name}")


def check_stale_text(all_docs: str) -> None:
    """Text describing an interface the project no longer ships."""
    for stale in (
        "libnvecdclient.a",
        "--log-level",
        "nvecd-test",
        "result->ids",
        "result->scores",
        "err->message",
        "VECSET item1 3 0.1 0.5 0.8",
        "--target uninstall",
        # The dispatcher formats every failure as "ERROR <message>"; the
        # Redis-style spelling was never on the wire. "(error) " is checked
        # separately, because nvecd-cli really does render a failure that way.
        "-ERR ",
        # The client headers state that a handle is shareable across threads
        # and serializes commands internally. Telling readers otherwise pushes
        # them into a connection-per-thread workaround they do not need.
        "not thread-safe",
        "one client instance per thread",
        "スレッドセーフではありません",
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


def check_error_spelling(source_root: Path, docs: list[Path]) -> None:
    """"(error) " belongs to nvecd-cli output, never to a wire transcript.

    The CLI prints a failed command as "(error) <message>" (src/cli), so a
    transcript of it legitimately carries that spelling. The server itself only
    ever writes "ERROR <message>", so the same text under a raw socket or an
    HTTP example is a promise it cannot keep. A doc alternates command blocks
    and output blocks, so the transport is whatever the nearest preceding
    command block used.
    """
    for path in docs:
        transport_is_cli = False
        for _language, block in FENCED_BLOCK.findall(path.read_text(encoding="utf-8")):
            if "nvecd-cli" in block or "nvecd>" in block:
                transport_is_cli = True
            elif re.search(r"\b(?:nc|telnet|curl|socat)\b", block):
                transport_is_cli = False
            if "(error) " in block and not transport_is_cli:
                fail(
                    f"{path.relative_to(source_root)} spells a failure "
                    '"(error) " outside nvecd-cli output; the wire sends '
                    '"ERROR <message>"'
                )


def check_required_text(source_root: Path) -> None:
    """Interfaces each guide must document because operators depend on them."""
    for language in ("en", "ja"):
        client_doc = (
            source_root / "docs" / language / "client-library.md"
        ).read_text(encoding="utf-8")
        for required in (
            "nvecd::client",
            "result.error().message()",
            "total_commands_processed",
            "ctypes.c_uint16",
            "ctypes.c_size_t",
            # Indexing into the C response array and reading an item, whatever
            # the example names the handle. The contract is the access pattern,
            # not the local variable.
            "results[i].id",
        ):
            if required not in client_doc:
                fail(f"{language} client guide is missing: {required}")

        installation_doc = (
            source_root / "docs" / language / "installation.md"
        ).read_text(encoding="utf-8")
        for required in ("--password-file", "--password-env"):
            if required not in installation_doc:
                fail(f"{language} installation guide is missing: {required}")


def check_cli_help(cli_path: Path) -> None:
    """The CLI advertises the options the installation guide tells people to use."""
    result = subprocess.run(
        [str(cli_path), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode != 0:
        fail(f"nvecd-cli --help returned {result.returncode}")
        return
    for required in ("--password-file FILE", "--password-env NAME", "--wait-ready"):
        if required not in result.stdout:
            fail(f"CLI help is missing: {required}")


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "documentation contract failure: expected <source-root> <nvecd-cli>",
            file=sys.stderr,
        )
        return 1

    source_root = Path(sys.argv[1])
    cli_path = Path(sys.argv[2])
    docs = sorted((source_root / "docs").glob("**/*.md"))
    all_docs = "\n".join(path.read_text(encoding="utf-8") for path in docs)

    check_stale_text(all_docs)
    check_error_spelling(source_root, docs)
    check_required_text(source_root)
    check_generated_docs(source_root)
    check_schema_wiring(source_root)
    check_wire_vocabulary(source_root)
    check_client_api_signatures(source_root)
    check_language_parity(source_root)
    check_error_code_reachability(source_root)
    check_cli_help(cli_path)

    if failures:
        print(
            f"documentation contract failures ({len(failures)}):", file=sys.stderr
        )
        for message in failures:
            print(f"  {message}", file=sys.stderr)
        return 1

    print(f"documentation contracts verified across {len(docs)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
