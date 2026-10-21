# PyGhidra batch query tool for Game.exe static analysis.
# Usage: python tools/kimi_ghidra_batch.py <command.json>
# Requires: pip install pyghidra; env GHIDRA_INSTALL_DIR; existing project
#           ghidra-proj/dhsh containing Game.exe (created by analyzeHeadless -import).
# command.json:
#   {
#     "out": "output.json",              # required, result path
#     "dump_functions": true,            # dump all functions (addr,name,size) to out+".functions.txt"
#     "decompile": ["0x427aa0", ...],    # decompile functions (hex addr or name)
#     "xrefs_to": ["0xaa9e64", ...],     # list references TO these addresses
#     "strings": [".core", ...],         # defined strings containing substring, with xrefs
#     "grep_decompiled": {               # decompile functions in range, keep matches
#        "start": "0x410000", "end": "0x440000", "patterns": ["\\+ 0x10\\)"] },
#     "function_range": ["0x410000","0x440000"]   # dump only functions in range
#   }
import json
import re
import sys
import traceback

PROJ_DIR = r"D:\ai-work-flow\rea-dhsh\ghidra-proj"
PROJ_NAME = "dhsh"
PROGRAM_PATH = "/Game.exe"


def main():
    cmd_path = sys.argv[1]
    with open(cmd_path, "rb") as fh:
        cmd = json.load(fh)

    import pyghidra
    pyghidra.start()

    from ghidra.base.project import GhidraProject
    from ghidra.app.decompiler import DecompInterface
    from ghidra.program.util import DefinedDataIterator
    from ghidra.util.task import ConsoleTaskMonitor
    from java.lang import Long

    project = GhidraProject.openProject(PROJ_DIR, PROJ_NAME, True)
    program = project.openProgram("/", "Game.exe", False)
    monitor = ConsoleTaskMonitor()

    try:
        space = program.getAddressFactory().getDefaultAddressSpace()
        fm = program.getFunctionManager()
        rm = program.getReferenceManager()
        listing = program.getListing()

        def to_addr(s):
            s = str(s)
            return space.getAddress(Long.parseLong(s[2:], 16) if s.lower().startswith("0x") else Long.parseLong(s))

        def fmt(a):
            return "0x%x" % a.getOffset()

        def func_at(addr):
            f = fm.getFunctionAt(addr)
            if f is None:
                f = fm.getFunctionContaining(addr)
            return f

        def decompile_one(di, sel):
            addr = to_addr(sel)
            f = func_at(addr)
            if f is None:
                return {"select": sel, "error": "no function at %s" % fmt(addr)}
            out = {"select": sel, "entry": fmt(f.getEntryPoint()), "name": f.getName(),
                   "body": fmt(f.getBody().getMinAddress()) + ".." + fmt(f.getBody().getMaxAddress())}
            try:
                res = di.decompileFunction(f, 120, monitor)
                if res and res.getDecompiledFunction():
                    out["c"] = res.getDecompiledFunction().getC()
                else:
                    out["c_error"] = str(res.getErrorMessage()) if res else "null result"
            except Exception:
                out["c_error"] = traceback.format_exc()
            calls = set()
            datarefs = set()
            for ins in listing.getInstructions(f.getBody(), True):
                for ref in ins.getReferencesFrom():
                    if ref.getReferenceType().isCall():
                        tf = func_at(ref.getToAddress())
                        calls.add(fmt(ref.getFromAddress()) + " -> " +
                                  (tf.getName() + "@" + fmt(tf.getEntryPoint()) if tf else fmt(ref.getToAddress())))
                    else:
                        datarefs.add(fmt(ref.getFromAddress()) + " -> " + fmt(ref.getToAddress()) +
                                     " " + str(ref.getReferenceType()))
            out["calls"] = sorted(calls)
            out["data_refs"] = sorted(datarefs)
            callers = set()
            for ref in rm.getReferencesTo(f.getEntryPoint()):
                if ref.getReferenceType().isCall():
                    cf = func_at(ref.getFromAddress())
                    callers.add(fmt(ref.getFromAddress()) + " <- " +
                                (cf.getName() + "@" + fmt(cf.getEntryPoint()) if cf else fmt(ref.getFromAddress())))
            out["called_by"] = sorted(callers)
            return out

        def xrefs_to(sel):
            addr = to_addr(sel)
            rows = []
            for ref in rm.getReferencesTo(addr):
                f = func_at(ref.getFromAddress())
                rows.append({"from": fmt(ref.getFromAddress()),
                             "type": str(ref.getReferenceType()),
                             "in_function": (f.getName() + "@" + fmt(f.getEntryPoint())) if f else None})
            sym = program.getSymbolTable().getPrimarySymbol(addr)
            return {"select": sel, "address": fmt(addr),
                    "symbol": str(sym) if sym else None, "refs": rows}

        def find_strings(substrings):
            hits = []
            subs = [s.encode("utf-8") for s in substrings]
            for data in listing.getDefinedData(True):
                try:
                    if not data.isString():
                        continue
                    v = data.getValue()
                except Exception:
                    continue
                if v is None:
                    continue
                bs = str(v).encode("utf-8", "replace")
                if any(s in bs for s in subs):
                    addr = data.getAddress()
                    refs = []
                    for ref in rm.getReferencesTo(addr):
                        f = func_at(ref.getFromAddress())
                        refs.append({"from": fmt(ref.getFromAddress()),
                                     "in_function": (f.getName() + "@" + fmt(f.getEntryPoint())) if f else None})
                    hits.append({"address": fmt(addr), "value": str(v)[:200], "refs": refs})
            return hits

        result = {"program": program.getName(), "image_base": fmt(program.getImageBase())}

        if cmd.get("dump_functions") or cmd.get("function_range"):
            rows = []
            if cmd.get("function_range"):
                start, end = to_addr(cmd["function_range"][0]), to_addr(cmd["function_range"][1])
                for f in fm.getFunctions(start, True):
                    if f.getEntryPoint().compareTo(end) > 0:
                        break
                    rows.append((f.getEntryPoint().getOffset(), f.getName(), f.getBody().getNumAddresses()))
            else:
                for f in fm.getFunctions(True):
                    rows.append((f.getEntryPoint().getOffset(), f.getName(), f.getBody().getNumAddresses()))
            rows.sort()
            fnpath = cmd["out"] + ".functions.txt"
            with open(fnpath, "w", encoding="utf-8") as fh:
                for off, name, size in rows:
                    fh.write("0x%x\t%s\t%d\n" % (off, name, size))
            result["function_count"] = len(rows)
            result["functions_file"] = fnpath

        di = DecompInterface()
        di.openProgram(program)
        try:
            if cmd.get("decompile"):
                result["decompile"] = [decompile_one(di, s) for s in cmd["decompile"]]
            if cmd.get("grep_decompiled"):
                g = cmd["grep_decompiled"]
                start, end = to_addr(g["start"]), to_addr(g["end"])
                pats = [re.compile(p) for p in g["patterns"]]
                matches = []
                count = 0
                for f in fm.getFunctions(start, True):
                    if f.getEntryPoint().compareTo(end) > 0:
                        break
                    count += 1
                    try:
                        res = di.decompileFunction(f, 60, monitor)
                        if not res or not res.getDecompiledFunction():
                            continue
                        c = res.getDecompiledFunction().getC()
                    except Exception:
                        continue
                    hit = [p.pattern for p in pats if p.search(c)]
                    if hit:
                        matches.append({"entry": fmt(f.getEntryPoint()), "name": f.getName(),
                                        "matched": hit, "c": c})
                result["grep_decompiled"] = {"scanned": count, "match_count": len(matches),
                                             "matches": matches}
        finally:
            di.dispose()

        if cmd.get("xrefs_to"):
            result["xrefs_to"] = [xrefs_to(s) for s in cmd["xrefs_to"]]
        if cmd.get("disasm"):
            dr = []
            for rng in cmd["disasm"]:
                start, end = to_addr(rng[0]), to_addr(rng[1])
                lines = []
                for ins in listing.getInstructions(start, True):
                    if ins.getAddress().compareTo(end) > 0:
                        break
                    lines.append(fmt(ins.getAddress()) + ": " + str(ins))
                dr.append({"range": rng, "text": "\n".join(lines)})
            result["disasm"] = dr
        if cmd.get("search_bytes"):
            sb = []
            mem = program.getMemory()
            for hexpat in cmd["search_bytes"]:
                mask = None
                if isinstance(hexpat, dict):
                    mask = bytes(bytearray(int(x, 16) for x in hexpat["mask"].split()))
                    hexpat = hexpat["bytes"]
                needle = bytes(bytearray(int(x, 16) for x in hexpat.split()))
                hits = []
                for block in mem.getBlocks():
                    if not block.isExecute():
                        continue
                    found = mem.findBytes(block.getStart(), block.getEnd(), needle, mask, True, monitor)
                    while found is not None:
                        hits.append(fmt(found))
                        f = func_at(found)
                        if f:
                            hits[-1] += " in " + f.getName() + "@" + fmt(f.getEntryPoint())
                        nxt = found.add(1)
                        if nxt.compareTo(block.getEnd()) > 0:
                            break
                        found = mem.findBytes(nxt, block.getEnd(), needle, None, True, monitor)
                sb.append({"pattern": hexpat, "hits": hits})
            result["search_bytes"] = sb
        if cmd.get("read_dwords"):
            rd = []
            mem = program.getMemory()
            for sel, count in cmd["read_dwords"]:
                addr = to_addr(sel)
                rows = []
                for i in range(count):
                    try:
                        v = mem.getInt(addr.add(i * 4)) & 0xffffffff
                    except Exception:
                        rows.append({"index": i, "error": "unmapped"})
                        continue
                    ta = space.getAddress(v)
                    f = func_at(ta)
                    sym = program.getSymbolTable().getPrimarySymbol(ta)
                    rows.append({"index": i, "offset": "0x%x" % (i * 4), "value": "0x%x" % v,
                                 "function": (f.getName() + "@" + fmt(f.getEntryPoint())) if f else None,
                                 "symbol": str(sym) if sym else None})
                rd.append({"address": sel, "rows": rows})
            result["read_dwords"] = rd
        if cmd.get("strings"):
            result["strings"] = find_strings(cmd["strings"])

        with open(cmd["out"], "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1)
        print("kimi_ghidra_batch: wrote %s" % cmd["out"])
    finally:
        project.close()


if __name__ == "__main__":
    main()
