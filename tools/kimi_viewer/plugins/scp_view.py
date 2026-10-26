"""SCP 结构化预览插件：窗口树 + include/脚本/警告摘要 + 原始文本。

只读解析（dhsh.formats.scp.parse_scp），不执行任何脚本。
"""
from __future__ import annotations

from dhsh.formats.scp import decode_scp, iter_nodes, parse_scp

MAX_CHARS = 2_000_000
MAX_VALUE = 160


def _props_lines(node, indent: str) -> list[str]:
    lines = []
    for key, value in node.props:
        v = value if len(value) <= MAX_VALUE else value[:MAX_VALUE] + "…"
        lines.append(f"{indent}{key} = {v}")
    return lines


def _walk(node, depth: int, out: list[str]):
    indent = "  " * depth
    head = f"{indent}{node.kind} {node.type}  (line {node.line})"
    if node.bgpic:
        head += f"  [BGPIC {node.bgpic.get('path', '')}]"
    out.append(head)
    out.extend(_props_lines(node, indent + "  "))
    for child in node.children:
        _walk(child, depth + 1, out)


class SCPPlugin:
    name = "SCP 界面脚本"
    exts = (".scp",)

    def load(self, payload: bytes, raw: bytes, entry, src) -> dict:
        scp = parse_scp(payload)
        n_nodes = sum(1 for _ in iter_nodes(scp.windows))
        lines = [
            f"SCP 结构化预览：{entry.path}",
            f"编码={scp.encoding}  顶层窗口={len(scp.windows)}  节点总数={n_nodes}  "
            f"include={len(scp.includes)}  脚本块={len(scp.scripts)}  "
            f"警告={len(scp.warnings)}",
            "",
        ]
        if scp.includes:
            lines.append("== include ==")
            lines.extend(scp.includes)
            lines.append("")
        if scp.props:
            lines.append("== 顶层属性 ==")
            for k, v in scp.props:
                lines.append(f"{k} = {v[:MAX_VALUE]}")
            lines.append("")
        lines.append("== 窗口树 ==")
        for win in scp.windows:
            _walk(win, 0, lines)
        if scp.scripts:
            lines.append("")
            lines.append("== 内嵌脚本 ==")
            for i, script in enumerate(scp.scripts):
                lines.append(f"-- script[{i}]，{script.count(chr(10)) + 1} 行 --")
                lines.append(script)
        if scp.warnings:
            lines.append("")
            lines.append("== 解析警告 ==")
            lines.extend(scp.warnings)
        text, _ = decode_scp(payload)
        lines.append("")
        lines.append("== 原始文本 ==")
        lines.append(text)
        out = "\n".join(lines)
        truncated = len(out) > MAX_CHARS
        if truncated:
            out = out[:MAX_CHARS]
        meta = {"编码": scp.encoding, "节点数": n_nodes,
                "include": len(scp.includes), "脚本块": len(scp.scripts),
                "警告": len(scp.warnings), "插件": self.name}
        if truncated:
            meta["截断"] = f"仅显示前 {MAX_CHARS:,} 字符"
        return {"kind": "text", "text": out, "meta": meta}


from . import register  # noqa: E402

register(SCPPlugin())
