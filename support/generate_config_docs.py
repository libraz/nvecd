#!/usr/bin/env python3
"""Render the configuration reference and the example config from the schema.

`src/config/config-schema.json` is the single authority for every claim about a
configuration key: its type, default, accepted range and prose description in
both English and Japanese. This script renders that authority into the two
artefacts operators read, so neither can drift from it:

* `examples/config.yaml` -- rendered in full.
* the option tables in `docs/{en,ja}/configuration.md` -- rendered into the
  regions delimited by `<!-- BEGIN GENERATED: options <section> -->`.

Run with `--check` to verify the artefacts are current without writing.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

GENERATED_BANNER = {
    "en": "Generated from src/config/config-schema.json by support/generate_config_docs.py.",
    "ja": "Generated from src/config/config-schema.json by support/generate_config_docs.py.",
}

TYPE_NAMES = {
    "integer": "int",
    "number": "float",
    "string": "string",
    "boolean": "bool",
    "array": "list",
}

TABLE_HEADERS = {
    "en": ("| Option | Type | Default | Description |\n"
           "|--------|------|---------|-------------|\n"),
    "ja": ("| オプション | 型 | 既定値 | 説明 |\n"
           "|--------|------|---------|-------------|\n"),
}

BLOCK_PATTERN = re.compile(
    r"(<!-- BEGIN GENERATED: options (?P<section>[\w.]+) -->\n)"
    r".*?"
    r"(?P<end><!-- END GENERATED: options (?P=section) -->)",
    re.DOTALL,
)


def leaf_properties(node: dict) -> dict:
    """Return the direct properties of a schema node, empty when it has none."""
    return node.get("properties", {})


def is_leaf(node: dict) -> bool:
    """Report whether a schema node describes a value rather than a subtree."""
    return "properties" not in node


def walk_sections(schema: dict):
    """Yield (dotted path, node) for every node that owns leaf properties."""
    def visit(node: dict, dotted: str):
        props = leaf_properties(node)
        if any(is_leaf(child) for child in props.values()):
            yield dotted, node
        for name, child in props.items():
            if not is_leaf(child):
                yield from visit(child, f"{dotted}.{name}" if dotted else name)

    yield from visit(schema, "")


def description(node: dict, language: str) -> str:
    """Return the description of a node in the requested language."""
    if language == "ja":
        return node.get("description_ja", node.get("description", ""))
    return node.get("description", "")


def render_default(node: dict) -> str:
    """Render a schema default as it appears in a documentation table."""
    if "default" not in node:
        return "--"
    value = node["default"]
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return f'"{value}"'
    if isinstance(value, list):
        return "[]" if not value else json.dumps(value)
    return json.dumps(value)


def render_constraints(node: dict, quote: str = "`") -> str:
    """Render the accepted range or enumeration of a node, if it has one."""
    if "enum" in node:
        return " ".join(f"{quote}{value}{quote}" for value in node["enum"])
    low = node.get("minimum")
    strict_low = node.get("exclusiveMinimum")
    high = node.get("maximum")
    if low is not None and high is not None:
        return f"{json.dumps(low)}-{json.dumps(high)}"
    if low is not None:
        return f">= {json.dumps(low)}"
    if strict_low is not None:
        return f"> {json.dumps(strict_low)}"
    if high is not None:
        return f"<= {json.dumps(high)}"
    return ""


def render_table(node: dict, language: str) -> str:
    """Render the option table for one schema section."""
    rows = [TABLE_HEADERS[language]]
    for name, child in leaf_properties(node).items():
        if not is_leaf(child):
            continue
        type_name = TYPE_NAMES.get(child.get("type", ""), child.get("type", ""))
        constraints = render_constraints(child)
        text = description(child, language)
        if constraints:
            text = f"{text} ({constraints})"
        rows.append(
            f"| `{name}` | {type_name} | {render_default(child)} | {text} |\n"
        )
    return "".join(rows)


def render_yaml_value(node: dict) -> list[str]:
    """Render the example value of a leaf as YAML lines."""
    value = node.get("example", node.get("default"))
    if isinstance(value, bool):
        return [f" {'true' if value else 'false'}"]
    if isinstance(value, str):
        return [f' "{value}"']
    if isinstance(value, list):
        if not value:
            return [" []"]
        return ["\n"] + [f'  - "{item}"' for item in value]
    if isinstance(value, float):
        return [f" {value!r}"]
    if value is None:
        return [" null"]
    return [f" {value}"]


def render_yaml(schema: dict) -> str:
    """Render the complete example configuration file."""
    lines = [
        "# nvecd configuration example\n",
        f"#\n# {GENERATED_BANNER['en']}\n",
        "# Copy this file and edit it. Every option carries its default unless\n"
        "# the comment says the value shown is an example instead.\n",
        "#\n# Reference: docs/en/configuration.md, docs/ja/configuration.md\n",
    ]

    def emit(node: dict, indent: int):
        for name, child in leaf_properties(node).items():
            pad = " " * indent
            if is_leaf(child):
                text = description(child, "en")
                constraints = render_constraints(child, quote="")
                if constraints:
                    text = f"{text} [{constraints}]"
                lines.append(f"{pad}# {text}\n")
                if "example" in child:
                    lines.append(
                        f"{pad}# Example value; the default is "
                        f"{render_default(child)}.\n"
                    )
                value = "".join(render_yaml_value(child))
                if value.startswith("\n"):
                    lines.append(f"{pad}{name}:\n")
                    for item in value[1:].split("\n"):
                        lines.append(f"{pad}{item}\n")
                else:
                    lines.append(f"{pad}{name}:{value}\n")
            else:
                lines.append(f"\n{pad}# {description(child, 'en')}\n")
                lines.append(f"{pad}{name}:\n")
                emit(child, indent + 2)

    for name, child in leaf_properties(schema).items():
        lines.append(f"\n# {description(child, 'en')}\n")
        lines.append(f"{name}:\n")
        emit(child, 2)

    return "".join(lines)


def render_markdown(schema: dict, text: str, language: str) -> str:
    """Replace every generated option table in a configuration guide."""
    sections = dict(walk_sections(schema))

    def replace(match: re.Match) -> str:
        name = match.group("section")
        if name not in sections:
            raise SystemExit(f"unknown schema section in generated block: {name}")
        table = render_table(sections[name], language)
        return f"{match.group(1)}{table}{match.group('end')}"

    return BLOCK_PATTERN.sub(replace, text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when a generated artefact is out of date instead of writing it",
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root (defaults to the checkout this script lives in)",
    )
    args = parser.parse_args()

    root: Path = args.source_root
    schema = json.loads(
        (root / "src" / "config" / "config-schema.json").read_text(encoding="utf-8")
    )

    artefacts = {root / "examples" / "config.yaml": render_yaml(schema)}
    for language in ("en", "ja"):
        path = root / "docs" / language / "configuration.md"
        artefacts[path] = render_markdown(
            schema, path.read_text(encoding="utf-8"), language
        )

    stale = []
    for path, content in artefacts.items():
        if path.read_text(encoding="utf-8") == content:
            continue
        stale.append(path)
        if not args.check:
            path.write_text(content, encoding="utf-8")

    if args.check and stale:
        for path in stale:
            print(
                f"out of date with src/config/config-schema.json: "
                f"{path.relative_to(root)}",
                file=sys.stderr,
            )
        print(
            "regenerate with: python3 support/generate_config_docs.py",
            file=sys.stderr,
        )
        return 1

    if not args.check:
        for path in stale:
            print(f"regenerated {path.relative_to(root)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
