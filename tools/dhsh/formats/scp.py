"""SCP UI script parser.

SCP files are line-oriented GBK/GB18030 (some UTF-8) text used by the
Hero2D UI.  Observed grammar:

    // comment
    #include "config/ui/foo.scp"
    WINDOW TYPE = WIN
        NAME = "barwnd"
        RECT = 0,582,800,600
        BMP = "ui/bar/inputbg_800.a16"
        WINDOW TYPE = BUTTON
            NAME = "bar_close"
            MESSAGE FSM_BTN_MSG 1 CODE "OnChatClick"
        END
        FIXEDPART TYPE = EDIT
            ...
        END
    END

Blocks start with ``WINDOW`` or ``FIXEDPART`` (nested), end with ``END``.
Other lines are ``KEY = value`` or ``KEY token ...`` (e.g. MESSAGE).
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field


def decode_scp(data: bytes) -> tuple[str, str]:
    for enc in ("utf-8-sig", "gb18030", "gbk"):
        try:
            return data.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return data.decode("latin1"), "latin1"


def strip_comment(line: str) -> str:
    """Remove // outside double quotes."""
    in_quote = False
    i = 0
    while i < len(line) - 1:
        c = line[i]
        if c == '"':
            in_quote = not in_quote
        elif not in_quote and c == "/" and line[i + 1] == "/":
            return line[:i]
        i += 1
    return line


INCLUDE_RE = re.compile(r'#\s*include\s+"([^"]+)"', re.IGNORECASE)
BLOCK_RE = re.compile(r"^(WINDOW|FIXEDPART)\b(.*)$", re.IGNORECASE)
TYPE_RE = re.compile(r"\bTYPE\s*=\s*([^\s]+)", re.IGNORECASE)
KV_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*(.*)|\s+(.*))?$")


def split_brace_fields(inner: str) -> list[str]:
    """Split ``"A" = "b"; C=1`` on semicolons outside double quotes."""
    parts: list[str] = []
    buf: list[str] = []
    in_quote = False
    for c in inner:
        if c == '"':
            in_quote = not in_quote
            buf.append(c)
        elif c == ";" and not in_quote:
            parts.append("".join(buf))
            buf = []
        else:
            buf.append(c)
    parts.append("".join(buf))
    return parts


def parse_bgpic(value: str) -> dict[str, str] | None:
    """Parse a ``BGPIC = { "BGTYPE" = ...; "SECTION" = ... }`` value.

    Observed keys: BGTYPE, SECTION, TEXT, FONTTYPE, TITLE1..TITLE9,
    TEXTCOLOR, DRAWPARENT, FORCEWIDTH/FORCEHEIGHT, XOFFSET/YOFFSET,
    HOFFSET, YSPACE.  Returns None when the value is not a brace block.
    """
    v = value.strip()
    if not (v.startswith("{") and v.endswith("}")):
        return None
    fields: dict[str, str] = {}
    for part in split_brace_fields(v[1:-1]):
        part = part.strip()
        if not part or "=" not in part:
            continue
        key, val = part.split("=", 1)
        key = key.strip().strip('"').upper()
        if not key:
            continue
        fields[key] = val.strip().strip('"')
    return fields or None


def _odd_quotes(value: str) -> bool:
    count = 0
    escaped = False
    for c in value:
        if escaped:
            escaped = False
        elif c == "\\":
            escaped = True
        elif c == '"':
            count += 1
    return count % 2 == 1


@dataclass
class SCPNode:
    kind: str                     # WINDOW / FIXEDPART
    type: str                     # WIN / BUTTON / ...
    line: int
    props: list[tuple[str, str]] = field(default_factory=list)
    children: list["SCPNode"] = field(default_factory=list)
    bgpic: dict[str, str] | None = None


@dataclass
class SCPFile:
    includes: list[str]
    props: list[tuple[str, str]]
    windows: list[SCPNode]
    warnings: list[str]
    encoding: str
    preproc: list[tuple[str, str]] = field(default_factory=list)
    scripts: list[str] = field(default_factory=list)
    free_text: list[str] = field(default_factory=list)
    bgpic: dict[str, str] | None = None


def last_bgpic(props: list[tuple[str, str]]) -> dict[str, str] | None:
    """Return the parsed last BGPIC property in *props* (or None)."""
    for key, value in reversed(props):
        if key == "BGPIC":
            return parse_bgpic(value)
    return None


# The client ships 800x600 UI assets, so WINWIDTH800 is treated as defined.
PREDEFINED = {"WINWIDTH800"}


def parse_scp(data: bytes) -> SCPFile:
    text, encoding = decode_scp(data)
    includes: list[str] = []
    root_props: list[tuple[str, str]] = []
    roots: list[SCPNode] = []
    stack: list[SCPNode] = []
    warnings: list[str] = []
    preproc: list[tuple[str, str]] = []
    scripts: list[str] = []
    free_text: list[str] = []
    cond_stack: list[bool] = []
    active = True
    in_script = False
    script_lines: list[str] = []
    in_block_comment = False
    pending_target: tuple[list[tuple[str, str]], int] | None = None

    for lineno, raw in enumerate(text.splitlines(), 1):
        stripped = raw.strip()
        if in_script:
            if stripped.lower() == "#endscript":
                scripts.append("\n".join(script_lines))
                script_lines = []
                in_script = False
            else:
                script_lines.append(raw)
            continue
        if in_block_comment:
            if "*/" in raw:
                in_block_comment = False
            continue
        if stripped.startswith("/*"):
            if "*/" not in stripped:
                in_block_comment = True
            continue
        if pending_target is not None:
            target, idx = pending_target
            key, value = target[idx]
            value = value + "\n" + raw.strip()
            target[idx] = (key, value)
            if not _odd_quotes(value):
                pending_target = None
            continue
        line = strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("#"):
            rest = line[1:].lstrip()
            parts = rest.split(None, 1)
            name = "#" + (parts[0].lower() if parts else "")
            arg = parts[1].strip() if len(parts) > 1 else ""
            if name == "#script":
                preproc.append((name, arg))
                in_script = True
                script_lines = []
                continue
            if name == "#endscript":
                warnings.append(f"line {lineno}: #endscript without #script")
                continue
            if name in ("#ifdef", "#ifndef"):
                cond = arg.upper() in PREDEFINED
                if name == "#ifndef":
                    cond = not cond
                preproc.append((name, arg))
                cond_stack.append(cond)
                active = active and cond
                continue
            if name == "#else":
                preproc.append((name, ""))
                if cond_stack:
                    cond_stack[-1] = not cond_stack[-1]
                    parent = all(cond_stack[:-1])
                    active = parent and cond_stack[-1]
                else:
                    warnings.append(f"line {lineno}: #else without #if")
                continue
            if name == "#endif":
                preproc.append((name, ""))
                if cond_stack:
                    cond_stack.pop()
                    active = all(cond_stack)
                else:
                    warnings.append(f"line {lineno}: #endif without #if")
                continue
            m = INCLUDE_RE.match(line)
            if m:
                if active:
                    includes.append(m.group(1).strip())
                continue
            preproc.append((name, arg))
            if name not in ("#define", "#undef", "#pragma"):
                warnings.append(f"line {lineno}: unknown directive {line[:60]!r}")
            continue
        if not active:
            continue
        upper = line.upper()
        if upper == "END":
            if stack:
                stack.pop()
            else:
                warnings.append(f"line {lineno}: END without block")
            continue
        m = BLOCK_RE.match(line)
        if m:
            kind = m.group(1).upper()
            tail = m.group(2)
            tm = TYPE_RE.search(tail)
            node = SCPNode(kind, tm.group(1) if tm else "", lineno)
            if stack:
                stack[-1].children.append(node)
            else:
                roots.append(node)
            stack.append(node)
            continue
        if line.startswith('"'):
            target = stack[-1].props if stack else root_props
            if target:
                key, value = target[-1]
                target[-1] = (key, value + " " + line)
            else:
                free_text.append(line)
            continue
        m = KV_RE.match(line)
        if m:
            key = m.group(1).upper()
            value = (m.group(2) if m.group(2) is not None else m.group(3) or "").strip()
            target = stack[-1].props if stack else root_props
            target.append((key, value))
            if _odd_quotes(value):
                pending_target = (target, len(target) - 1)
            continue
        free_text.append(line)

    if in_script:
        warnings.append("unclosed #script block")
        scripts.append("\n".join(script_lines))
    if cond_stack:
        warnings.append(f"unclosed #if blocks: {len(cond_stack)}")
    if stack:
        warnings.append(f"unclosed blocks: {len(stack)}")
    root_bgpic = last_bgpic(root_props)
    for node in iter_nodes(roots):
        node.bgpic = last_bgpic(node.props)
    return SCPFile(includes, root_props, roots, warnings, encoding,
                   preproc, scripts, free_text, root_bgpic)


def iter_nodes(nodes: list[SCPNode]):
    for node in nodes:
        yield node
        yield from iter_nodes(node.children)


def node_to_dict(node: SCPNode) -> dict:
    return {
        "kind": node.kind,
        "type": node.type,
        "line": node.line,
        "bgpic": node.bgpic,
        "props": node.props,
        "children": [node_to_dict(c) for c in node.children],
    }


def scp_to_dict(scp: SCPFile) -> dict:
    return {
        "encoding": scp.encoding,
        "bgpic": scp.bgpic,
        "includes": scp.includes,
        "preproc": scp.preproc,
        "scripts": scp.scripts,
        "free_text": scp.free_text,
        "props": scp.props,
        "windows": [node_to_dict(n) for n in scp.windows],
        "warnings": scp.warnings,
    }
