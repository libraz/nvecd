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
  in a string literal of the source that produces it, never only in a comment;
* the protocol error table lists exactly the ERROR messages a TCP client can
  receive, and the metric, cache-statistics and IVF figures match the code;
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

# Pages among WIRE_DOCS that also name on-disk and process mechanics. There a
# token may instead be an identifier in comment-free code, or a file magic that
# a 32-bit constant spells in little-endian byte order.
FORMAT_DOCS = ("persistence.md",)

# Sources whose error text reaches a TCP client as "ERROR <message>". A file
# paired with a function name contributes only that function's messages.
WIRE_ERROR_SOURCES: tuple[tuple[str, str | None], ...] = (
    ("src/server/command_parser.cpp", None),
    ("src/server/argument_validation.cpp", None),
    ("src/server/filter_parser.cpp", None),
    ("src/server/request_dispatcher.cpp", None),
    ("src/server/reactor_connection.cpp", None),
    ("src/server/nvecd_server.cpp", None),
    ("src/server/snapshot_coordinator.cpp", None),
    ("src/server/handlers/admin_handler.cpp", None),
    ("src/server/handlers/cache_handler.cpp", None),
    ("src/server/handlers/debug_handler.cpp", None),
    ("src/server/handlers/dump_handler.cpp", None),
    ("src/server/handlers/info_handler.cpp", None),
    ("src/server/handlers/variable_handler.cpp", None),
    ("src/config/runtime_variable_manager.cpp", None),
    ("src/events/id_validation.h", None),
    ("src/events/event_store.cpp", None),
    ("src/vectors/vector_store.cpp", None),
    ("src/similarity/similarity_engine.cpp", None),
    ("src/storage/snapshot_fork.cpp", None),
    ("src/utils/path_utils.h", "ValidateDumpPath"),
)

# Messages from WIRE_ERROR_SOURCES that the protocol error table does not list,
# because a TCP client cannot provoke them. "<>" stands for a computed part.
UNLISTED_ERROR_MESSAGES = (
    # Internal invariants and unwired components.
    "Cannot save snapshot: required stores not initialized",
    "Cannot save snapshot: server configuration is not available",
    "Cannot load snapshot: required stores not initialized",
    "Event apply diverged after WAL acceptance",
    "Event dedup state changed during acceptance",
    "EventStore not initialized",
    "Fork snapshot writer not initialized",
    "MetadataStore not initialized",
    "METASET carries no metadata",
    "No value resolver for variable: <>",
    "Not a search command: <>",
    "Not a write command: <>",
    "RuntimeVariableManager not initialized",
    "Server configuration is not available",
    "Server not initialized",
    "SimilarityEngine not initialized",
    "Unknown command",
    "Unknown command type",
    "Variable not implemented: <>",
    "VectorStore not initialized",
    "Write produced an unformattable outcome",
    # Values the parser rejects before the layer that would report them.
    "<> exceeds the finite float range",
    "Both event and vector searches failed for ID: <>",
    "Cannot normalize zero vector",
    "DEL score must be 0, got <>",
    "Invalid event type: <>",
    "Invalid filepath: must not be empty",
    "Invalid logging format (must be json/text): <>",
    "top_k exceeds maximum allowed: <>",
    "top_k must be positive",
    "Vector components must be finite",
    # Snapshot restore reasons, which reach the wire only inside
    # "Failed to load snapshot from <path>: <reason>".
    "Cannot restore dimension <> onto a store holding <>-dimensional vectors",
    "Restored dimension out of range: <>",
    # Lifecycle paths that are not requests.
    "Server already running",
    "Server is read-only; decay maintenance skipped",
)

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


def unescape(literal: str) -> str:
    """Decode the escapes of a C++ string literal body that matter to text."""
    controls = {"n": "\n", "r": "\r", "t": "\t", "0": "\0"}
    return re.sub(r"\\(.)", lambda m: controls.get(m.group(1), m.group(1)), literal)


def scan_cpp(text: str) -> tuple[str, list[str]]:
    """Split C++ source into comment-free code and its string literals.

    Literals stay in the returned code; only comments are dropped. A quote
    after a digit is a digit separator, not a character literal.
    """
    code: list[str] = []
    literals: list[str] = []
    index = 0
    while index < len(text):
        char = text[index]
        if text.startswith("//", index):
            end = text.find("\n", index)
            index = len(text) if end < 0 else end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = len(text) if end < 0 else end + 2
            continue
        raw = re.match(r'R"([^()\\\s]{0,16})\(', text[index:])
        if raw is not None and (index == 0 or not text[index - 1].isalnum()):
            terminator = ")" + raw.group(1) + '"'
            start = index + raw.end()
            end = text.find(terminator, start)
            end = len(text) if end < 0 else end
            literals.append(text[start:end])
            code.append(text[index : end + len(terminator)])
            index = end + len(terminator)
            continue
        if char == '"' or (char == "'" and not (index > 0 and text[index - 1].isdigit())):
            end = index + 1
            while end < len(text) and text[end] != char and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            if char == '"':
                literals.append(unescape(text[index + 1 : end]))
            code.append(text[index : end + 1])
            index = end + 1
            continue
        code.append(char)
        index += 1
    return "".join(code), literals


def balanced(text: str, start: int, opener: str = "(", closer: str = ")") -> int:
    """Index just past the bracket that closes the one before @p start.

    String and character literals are skipped, so a bracket inside a message
    does not end the span.
    """
    depth = 1
    index = start
    while index < len(text) and depth > 0:
        char = text[index]
        if char in "\"'" and not (char == "'" and text[index - 1].isdigit()):
            index += 1
            while index < len(text) and text[index] != char:
                index += 2 if text[index] == "\\" else 1
        elif char == opener:
            depth += 1
        elif char == closer:
            depth -= 1
        index += 1
    return index


def split_top_level(expression: str, separators: str) -> list[str]:
    """Split on separators that sit outside brackets and literals."""
    parts = [""]
    depth = 0
    index = 0
    while index < len(expression):
        char = expression[index]
        if char == '"':
            end = index + 1
            while end < len(expression) and expression[end] != '"':
                end += 2 if expression[end] == "\\" else 1
            parts[-1] += expression[index : end + 1]
            index = end + 1
            continue
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        scope = expression[index : index + 2] == "::" or (index > 0 and expression[index - 1] == ":")
        if depth == 0 and char in separators and not (char == ":" and scope):
            parts.append("")
            index += 1
            continue
        parts[-1] += char
        index += 1
    return [part.strip() for part in parts]


def message_templates(expression: str) -> list[str]:
    """Message text an expression can produce, with "<>" for computed parts.

    Each branch of a conditional is its own template. An expression that holds
    no literal forwards a message from elsewhere and yields nothing.
    """
    templates = []
    for branch in split_top_level(expression, "?:")[-2:] if "?" in expression else [expression]:
        text = ""
        has_literal = False
        for piece in split_top_level(branch, "+"):
            literal = re.fullmatch(r'(?:std::string\()?((?:"(?:[^"\\]|\\.)*"\s*)+)\)?', piece)
            if literal is None:
                text += "<>"
                continue
            has_literal = True
            text += "".join(
                unescape(part) for part in re.findall(r'"((?:[^"\\]|\\.)*)"', literal.group(1))
            )
        if has_literal:
            templates.append(text)
    return templates


def function_body(code: str, name: str) -> str:
    """The body of the function definition named @p name, or empty."""
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", code):
        after = balanced(code, match.end())
        brace = re.match(r"\s*(?:const\s*)?\{", code[after:])
        if brace is not None:
            open_index = after + brace.end()
            return code[open_index : balanced(code, open_index, "{", "}") - 1]
    return ""


def wire_error_templates(source_root: Path) -> dict[str, str]:
    """Every TCP error message template in WIRE_ERROR_SOURCES, with its file."""
    templates: dict[str, str] = {}
    for relative, function in WIRE_ERROR_SOURCES:
        code, _ = scan_cpp((source_root / relative).read_text(encoding="utf-8"))
        if function is not None:
            code = function_body(code, function)
            if not code:
                fail(f"WIRE_ERROR_SOURCES names a function {relative} does not define: {function}")
                continue
        expressions: list[str] = []
        for call in re.finditer(r"\b(MakeError|refuse|Refuse|FormatError)\(", code):
            arguments = split_top_level(code[call.end() : balanced(code, call.end()) - 1], ",")
            # MakeError takes the message second; the refusal helpers take it last.
            if call.group(1) != "MakeError":
                expressions.append(arguments[-1])
            elif len(arguments) > 1:
                expressions.append(arguments[1])
        expressions += re.findall(
            r'(?:\*message|\berror_msg)\s*=\s*((?:"(?:[^"\\]|\\.)*"|[^;"])+);', code
        )
        # A message held in a named constant is spelled where it is declared.
        constants = dict(
            re.findall(
                r'constexpr (?:const char\*|std::string_view) (k\w+) = ("(?:[^"\\]|\\.)*");', code
            )
        )
        for expression in expressions:
            expression = re.sub(
                r"\bk\w+\b", lambda m: constants.get(m.group(0), m.group(0)), expression
            )
            for template in message_templates(expression):
                templates.setdefault(template, relative)
        for literal in re.findall(r'"ERROR ([^"\\]+)(?:\\r\\n)?"', code):
            templates.setdefault(literal, relative)
    return templates


def template_pattern(template: str) -> re.Pattern:
    """Match documented text against a template whose "<>" parts vary."""
    return re.compile(".+".join(re.escape(part) for part in template.split("<>")))


def documented_error_messages(text: str) -> list[str]:
    """Messages in the first column of the protocol page's error table."""
    for block in re.findall(r"(?:^\|.*\n)+", strip_code(text), re.M):
        if "`Empty command`" not in block:
            continue
        messages = []
        for row in block.splitlines()[2:]:
            cell = re.split(r"(?<!\\)\|", row)[1]
            messages += [m.replace("\\|", "|") for m in re.findall(r"`([^`]+)`", cell)]
        return messages
    return []


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
    # except where the parser converts units or folds a legacy alias.
    field_names = {
        "cache.max_memory_mb": "max_memory_bytes",
        "similarity.ivf_enabled": "index_type",
    }
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
    code_parts = []
    literal_words: set[str] = set()
    for path in sorted((source_root / "src").rglob("*")):
        if not (path.is_file() and path.suffix in (".cpp", ".h", ".c")):
            continue
        code, literals = scan_cpp(path.read_text(encoding="utf-8", errors="ignore"))
        code_parts.append(code)
        for literal in literals:
            literal_words.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", literal))
    sources = "\n".join(code_parts)
    without_literals = re.sub(r'"(?:[^"\\\n]|\\.)*"', "", sources)
    identifiers = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", without_literals))
    magics = {
        bytes.fromhex(value)[::-1].decode("latin-1")
        for value in re.findall(r"constexpr uint32_t k\w*Magic = 0x([0-9A-Fa-f]{8})", sources)
    }

    def produced(token: str, name: str) -> bool:
        # A whole word of a string literal: text in a comment, or a longer word
        # that merely contains the token, is not output.
        if token in literal_words:
            return True
        return name in FORMAT_DOCS and (token in identifiers or token in magics)

    for language in ("en", "ja"):
        for name in WIRE_DOCS:
            path = source_root / "docs" / language / name
            text = path.read_text(encoding="utf-8")

            for token in sorted(backticked(text)):
                if not (FIELD_NAME.match(token) or STATUS_WORD.match(token)):
                    continue
                if not produced(token, name):
                    fail(
                        f"{language}/{name} documents a token no source produces: "
                        f"{token}"
                    )

            # Keys under "metadata" are the caller's own, not the server's.
            keys = set()
            for _language, block in FENCED_BLOCK.findall(text):
                block = re.sub(r'"metadata"\s*:\s*\{[^{}]*\}', '"metadata": {}', block)
                keys |= set(re.findall(r'"([a-z][a-z0-9_]*)"\s*:', block))
            for key in sorted(keys):
                if not produced(key, name):
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


def check_source_scanner() -> None:
    """Comment text never counts as output; literal text always does."""
    code, literals = scan_cpp(
        '// "comment_only"\n/* "block_only" */ x = "emitted_word: " + y; c = \'"\'; n = 1\'000;'
        ' s = R"(raw_word)"; t = "ERROR Tail\\r\\n";'
    )
    words = {w for literal in literals for w in re.findall(r"[A-Za-z_]\w*", literal)}
    if "comment_only" in code or {"comment_only", "block_only"} & words:
        fail("the source scanner reads comments as source")
    if not {"emitted_word", "raw_word", "Tail"} <= words:
        fail(f"the source scanner misses string literals: {sorted(words)}")
    if message_templates('flag ? "A " + name : std::string("B")') != ["A <>", "B"]:
        fail("message templates mishandle a conditional message")


def check_protocol_error_table(source_root: Path) -> None:
    """The protocol error table lists exactly the messages a TCP client can get.

    A documented row must be text some template in WIRE_ERROR_SOURCES produces,
    and every such template outside UNLISTED_ERROR_MESSAGES must have a row.
    """
    templates = wire_error_templates(source_root)
    patterns = {template: template_pattern(template) for template in templates}
    for template in UNLISTED_ERROR_MESSAGES:
        if template not in templates:
            fail(f"UNLISTED_ERROR_MESSAGES names a message no source sends: {template}")

    for language in ("en", "ja"):
        text = (source_root / "docs" / language / "protocol.md").read_text(encoding="utf-8")
        documented = documented_error_messages(text)
        if not documented:
            fail(f"{language}/protocol.md has no error table")
            continue
        for message in documented:
            if not any(pattern.fullmatch(message) for pattern in patterns.values()):
                fail(f"{language}/protocol.md lists an error no source sends: {message}")
        for template, pattern in sorted(patterns.items()):
            listed = any(pattern.fullmatch(message) for message in documented)
            if template in UNLISTED_ERROR_MESSAGES:
                if listed and "<>" not in template:
                    fail(f"UNLISTED_ERROR_MESSAGES names a documented message: {template}")
                continue
            if not listed:
                fail(
                    f"{language}/protocol.md error table is missing "
                    f"{template!r} ({templates[template]})"
                )


def source_code(source_root: Path, relative: str) -> str:
    """Comment-free text of one source file."""
    return scan_cpp((source_root / relative).read_text(encoding="utf-8"))[0]


def check_metrics_exposition(source_root: Path) -> None:
    """Documented metric names, types and help text match /metrics."""
    code = source_code(source_root, "src/server/http_server.cpp")
    body = function_body(code, "HandleMetrics")
    types = dict(re.findall(r'"# TYPE (nvecd_\w+) (\w+)\\n"', body))
    helps = dict(re.findall(r'"# HELP (nvecd_\w+) ([^"\\]+)\\n"', body))
    if not types:
        fail("cannot locate the TYPE lines HandleMetrics emits")
        return

    for language in ("en", "ja"):
        text = (source_root / "docs" / language / "http-api.md").read_text(encoding="utf-8")
        for _language, block in FENCED_BLOCK.findall(text):
            for name, kind in re.findall(r"^# TYPE (nvecd_\w+) (\w+)$", block, re.M):
                if types.get(name) != kind:
                    fail(
                        f"{language}/http-api.md shows '# TYPE {name} {kind}'; "
                        f"code: {types.get(name)}"
                    )
            for name, help_text in re.findall(r"^# HELP (nvecd_\w+) (.+)$", block, re.M):
                if helps.get(name) != help_text:
                    fail(
                        f"{language}/http-api.md shows '# HELP {name} {help_text}'; "
                        f"code: {helps.get(name)}"
                    )
        rows = dict(re.findall(r"^\| `(nvecd_\w+)` \| (\w+) \|", strip_code(text), re.M))
        for name, kind in sorted(rows.items()):
            if types.get(name) != kind:
                fail(f"{language}/http-api.md types {name} as {kind}; code: {types.get(name)}")
        for name in sorted(set(types) - set(rows)):
            fail(f"{language}/http-api.md metric table is missing {name}")


def check_cache_stats_fields(source_root: Path) -> None:
    """CACHE STATS samples and the /cache/stats body match the emitted fields."""
    handler = source_code(source_root, "src/server/handlers/cache_handler.cpp")
    tcp = re.findall(r'oss << "(\w+): "', function_body(handler, "HandleCacheStats"))
    http_code = source_code(source_root, "src/server/http_server.cpp")
    http = re.findall(r'response\["(\w+)"\]', function_body(http_code, "HandleCacheStats"))
    if not tcp or not http:
        fail("cannot locate the fields the two CACHE STATS handlers emit")
        return

    for path in sorted((source_root / "docs").rglob("*.md")):
        for _language, block in FENCED_BLOCK.findall(path.read_text(encoding="utf-8")):
            lines = [line for line in block.split("\n") if line.strip()]
            if "OK CACHE_STATS" not in lines[:3]:
                continue
            start = lines.index("OK CACHE_STATS") + 1
            shown = [line.split(":", 1)[0] for line in lines[start:] if line != "END"]
            if shown != tcp:
                fail(
                    f"{path.relative_to(source_root)} CACHE STATS sample differs "
                    f"from the handler: {shown}"
                )

    renamed = sorted(set(tcp) - set(http))
    for language in ("en", "ja"):
        text = (source_root / "docs" / language / "http-api.md").read_text(encoding="utf-8")
        section = re.search(r"^### `GET /cache/stats`\n(.*?)^#", text, re.M | re.S)
        if section is None:
            fail(f"{language}/http-api.md has no GET /cache/stats section")
            continue
        for _language, block in FENCED_BLOCK.findall(section.group(1)):
            shown = re.findall(r'^\s*"(\w+)"\s*:', block, re.M)
            if shown and sorted(shown) != sorted(http):
                fail(
                    f"{language}/http-api.md /cache/stats sample differs from the "
                    f"handler: {shown}"
                )
        for name in renamed:
            if f"`{name}`" not in section.group(1):
                fail(f"{language}/http-api.md does not say what TCP {name} is called over HTTP")


def check_ivf_numbers(source_root: Path) -> None:
    """Every number in the IVF section is a schema default or an IvfIndex constant."""
    schema_path = source_root / "src" / "config" / "config-schema.json"
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    similarity = schema["properties"]["similarity"]["properties"]
    allowed = {"0"} | {
        str(node["default"]) for key, node in similarity.items() if key.startswith("ivf_")
    }
    header = (source_root / "src" / "vectors" / "ivf_index.h").read_text(encoding="utf-8")
    allowed |= set(re.findall(r"static constexpr \w+ k\w+ = (\d+);", header))
    for language in ("en", "ja"):
        text = (source_root / "docs" / language / "vector-search.md").read_text(encoding="utf-8")
        section = re.search(r"^### ivf\n(.*?)^#", strip_code(text), re.M | re.S)
        if section is None:
            fail(f"{language}/vector-search.md has no ivf section")
            continue
        for number in re.findall(r"(?<![\w.])\d[\d,]*(?![\w.])", section.group(1)):
            if number.replace(",", "") not in allowed:
                fail(
                    f"{language}/vector-search.md ivf section states {number}, "
                    "which no IVF constant holds"
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
    for path in sorted(english_dir.rglob("*.md")):
        relative = path.relative_to(english_dir)
        counterpart = source_root / "docs" / "ja" / relative
        if not counterpart.exists():
            fail(f"docs/ja is missing a counterpart for {relative}")
            continue
        english = path.read_text(encoding="utf-8")
        japanese = counterpart.read_text(encoding="utf-8")

        if headings(english) != headings(japanese):
            fail(
                f"{relative}: heading structure differs between en and ja "
                f"({len(headings(english))} vs {len(headings(japanese))} headings)"
            )

        english_tokens = {t for t in backticked(english) if IDENTIFIER.match(t)}
        japanese_tokens = {t for t in backticked(japanese) if IDENTIFIER.match(t)}
        for token in sorted(english_tokens - japanese_tokens):
            fail(f"{relative}: documented in en but not in ja: {token}")
        for token in sorted(japanese_tokens - english_tokens):
            fail(f"{relative}: documented in ja but not in en: {token}")


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
    check_source_scanner()
    check_wire_vocabulary(source_root)
    check_protocol_error_table(source_root)
    check_metrics_exposition(source_root)
    check_cache_stats_fields(source_root)
    check_ivf_numbers(source_root)
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
