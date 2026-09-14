#!/usr/bin/env python3
"""
dbc2c.py - generate C signal tables from the LiveWire S2 DBC.

Input : can-db/livewire_s2_delmar_secondary.dbc
Output: src/gen/s2_dbc_gen.c  (message + signal tables)
        src/gen/s2_dbc_gen.h  (s2_signal_id_t enum with one entry per signal)

The DBC is a Vector .dbc with C++-style '//' comment lines interleaved (the
community database keeps its evidence inline).  Only BO_ / SG_ / VAL_ records
are consumed; comments are ignored except for the E2E-protected id lists in the
file header, which set the S2_MSG_E2E / S2_MSG_NO_E2E flags.

Usage: python3 tools/dbc2c.py [--dbc PATH] [--out-dir DIR] [--check]
  --check   exit 1 if the generated files differ from what is on disk
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DBC = os.path.join(ROOT, "can-db", "livewire_s2_delmar_secondary.dbc")
DEFAULT_OUT = os.path.join(ROOT, "src", "gen")

BO_RE = re.compile(r"^\s*BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)\s+(\w+)")
SG_RE = re.compile(
    r"^\s*SG_\s+(\w+)\s*(M|m\d+)?\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*"
    r"\(\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)\s*"
    r"\[\s*([-+0-9.eE]+)\s*\|\s*([-+0-9.eE]+)\s*\]\s*\"([^\"]*)\""
)
VAL_RE = re.compile(r"^\s*VAL_\s+(\d+)\s+(\w+)\s+(.*);")
VAL_PAIR_RE = re.compile(r"(-?\d+)\s+\"([^\"]*)\"")
HEX3_RE = re.compile(r"^(?:0x)?([0-9A-F]{3})$")   # upper-case hex only, as written in the header


class Signal:
    def __init__(self, name, mux, start, length, order, sign, factor, offset, vmin, vmax, unit):
        self.name = name
        self.mux = mux
        self.start = start
        self.length = length
        self.order = order      # 0 = big endian (Motorola), 1 = little endian (Intel)
        self.signed = sign == "-"
        self.factor = factor
        self.offset = offset
        self.min = vmin
        self.max = vmax
        self.unit = unit


class Message:
    def __init__(self, can_id, name, dlc, sender):
        self.id = can_id
        self.name = name
        self.dlc = dlc
        self.sender = sender
        self.signals = []
        self.values = {}   # signal name -> [(raw, text)]


def parse_id_list(text, start_marker, end_marker):
    """Extract 3-hex-digit ids between two markers in the header comment block."""
    start = text.find(start_marker)
    if start < 0:
        return None
    end = text.find(end_marker, start)
    if end < 0:
        return None
    span = text[start + len(start_marker):end]
    ids = []
    for tok in re.split(r"[\s,.]+", span.replace("//", " ")):
        m = HEX3_RE.match(tok)
        if m:
            ids.append(int(m.group(1), 16))
    return ids


def parse_dbc(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    messages = []
    current = None
    for lineno, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("//"):
            continue
        m = BO_RE.match(line)
        if m:
            can_id = int(m.group(1))
            if can_id > 0x7FF:
                if can_id == 0xC0000000:   # VECTOR__INDEPENDENT_SIG_MSG pseudo message
                    current = None
                    continue
                sys.exit(f"{path}:{lineno}: 29-bit id 0x{can_id:X} - the message table stores 11-bit ids only")
            if m.group(3) not in ("1", "2", "3", "4", "5", "6", "7", "8"):
                sys.exit(f"{path}:{lineno}: unsupported DLC {m.group(3)}")
            if any(x.name == m.group(2) for x in messages):
                sys.exit(f"{path}:{lineno}: duplicate message name {m.group(2)}")
            current = Message(can_id, m.group(2), int(m.group(3)), m.group(4))
            messages.append(current)
            continue
        m = SG_RE.match(line)
        if m:
            if current is None:
                if messages:
                    continue   # signal of the skipped pseudo message
                sys.exit(f"{path}:{lineno}: SG_ before any BO_")
            sig = Signal(
                m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5)),
                m.group(6), float(m.group(7)), float(m.group(8)), float(m.group(9)),
                float(m.group(10)), m.group(11),
            )
            if sig.length < 1 or sig.length > 64:
                sys.exit(f"{path}:{lineno}: bad signal length {sig.length}")
            if sig.start > 63:
                sys.exit(f"{path}:{lineno}: bad start bit {sig.start}")
            if sig.mux:
                sys.exit(f"{path}:{lineno}: multiplexed signal '{sig.name}' ({sig.mux}) - the firmware "
                         "decoder has no DBC multiplexer support; handle it in src/s2_overlay.c instead")
            if any(x.name == sig.name for x in current.signals):
                sys.exit(f"{path}:{lineno}: duplicate signal name '{sig.name}' in {current.name}")
            current.signals.append(sig)
            continue
        if line.startswith("SG_"):
            sys.exit(f"{path}:{lineno}: unparsable SG_ line: {line}")
        m = VAL_RE.match(line)
        if m:
            can_id = int(m.group(1))
            for msg in messages:
                if msg.id == can_id:
                    msg.values[m.group(2)] = [(int(r), t) for r, t in VAL_PAIR_RE.findall(m.group(3))]
            continue

    protected = parse_id_list(text, "PROTECTED IDs (D7=alive ctr, D8=CRC):", "Verified")
    unprotected = parse_id_list(text, "UNPROTECTED IDs (D7 & D8 CONSTANT padding, no per-frame integrity):", "A few carry")
    if not protected or not unprotected:
        sys.exit("could not locate the E2E PROTECTED / UNPROTECTED id lists in the DBC header")

    ids = [m.id for m in messages]
    dups = {i for i in ids if ids.count(i) > 1}
    if dups:
        sys.exit("duplicate BO_ ids: " + ", ".join(hex(i) for i in sorted(dups)))
    messages.sort(key=lambda m: m.id)
    return messages, set(protected), set(unprotected), text


def c_float(v):
    if v == int(v) and abs(v) < 1e9:
        return f"{int(v)}.0f"
    return f"{v!r}f"


def c_double(v):
    if v == int(v) and abs(v) < 1e15:
        return f"{int(v)}.0"
    return repr(v)


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def enum_name(msg, sig):
    return f"S2_SIG_{msg.name}_{sig.name}"


def generate(messages, protected, unprotected, dbc_text, dbc_path):
    version = ""
    m = re.match(r'\s*VERSION\s+"([^"]*)"', dbc_text)
    if m:
        version = m.group(1)
    rel = os.path.relpath(dbc_path, ROOT)
    total = sum(len(msg.signals) for msg in messages)

    # ---- header -------------------------------------------------------------
    h = []
    h.append("/* Generated by tools/dbc2c.py from %s - DO NOT EDIT. */" % rel)
    h.append("#pragma once")
    h.append("")
    h.append("#include \"s2_dbc.h\"")
    h.append("")
    h.append("#define S2_DBC_VERSION %s" % c_str(version))
    h.append("#define S2_DBC_MESSAGE_COUNT %du" % len(messages))
    h.append("#define S2_DBC_SIGNAL_COUNT %du" % total)
    h.append("")
    h.append("/* Global signal index: one entry per SG_ line, grouped by message, ascending id. */")
    h.append("typedef enum {")
    idx = 0
    for msg in messages:
        for sig in msg.signals:
            h.append("    %s = %d, /* 0x%03X %s%s */" % (
                enum_name(msg, sig), idx, msg.id, sig.name, (" [" + sig.unit + "]") if sig.unit else ""))
            idx += 1
    h.append("    S2_SIG__COUNT = %d," % idx)
    h.append("} s2_signal_id_t;")
    h.append("")
    h.append("/* Message ids as macros, for switch statements. */")
    for msg in messages:
        h.append("#define S2_ID_%s 0x%03Xu" % (msg.name, msg.id))
    h.append("")

    # ---- source ---------------------------------------------------------------
    c = []
    c.append("/* Generated by tools/dbc2c.py from %s - DO NOT EDIT. */" % rel)
    c.append("#include \"s2_dbc_gen.h\"")
    c.append("")
    for msg in messages:
        if not msg.signals:
            continue
        c.append("static const s2_signal_def_t sig_%03X[] = {" % msg.id)
        for sig in msg.signals:
            c.append("    { .name = %s, .unit = %s, .start_bit = %d, .length = %d, .byte_order = %s,"
                     % (c_str(sig.name), c_str(sig.unit), sig.start, sig.length,
                        "S2_ORDER_BIG_ENDIAN" if sig.order == 0 else "S2_ORDER_LITTLE_ENDIAN"))
            c.append("      .is_signed = %d, .factor = %s, .offset = %s, .min = %s, .max = %s },"
                     % (1 if sig.signed else 0, c_double(sig.factor), c_double(sig.offset),
                        c_float(sig.min), c_float(sig.max)))
        c.append("};")
        c.append("")
    c.append("const s2_message_def_t s2_dbc_messages[] = {")
    first = 0
    for msg in messages:
        flags = []
        if msg.id in protected:
            flags.append("S2_MSG_E2E")
        if msg.id in unprotected:
            flags.append("S2_MSG_NO_E2E")
        c.append("    { .id = 0x%03X, .dlc = %d, .flags = %s, .first_signal = %d, .signal_count = %d,"
                 % (msg.id, msg.dlc, " | ".join(flags) if flags else "0", first, len(msg.signals)))
        c.append("      .name = %s, .sender = %s, .signals = %s },"
                 % (c_str(msg.name), c_str(msg.sender), ("sig_%03X" % msg.id) if msg.signals else "NULL"))
        first += len(msg.signals)
    c.append("};")
    c.append("")
    c.append("const size_t s2_dbc_message_count = %d;" % len(messages))
    c.append("const size_t s2_dbc_signal_count = %d;" % total)
    c.append("")
    c.append("const s2_message_def_t *s2_dbc_find_message(uint32_t can_id)")
    c.append("{")
    c.append("    size_t lo = 0, hi = s2_dbc_message_count;")
    c.append("    while (lo < hi) {")
    c.append("        size_t mid = lo + (hi - lo) / 2;")
    c.append("        uint32_t id = s2_dbc_messages[mid].id;")
    c.append("        if (id == can_id) {")
    c.append("            return &s2_dbc_messages[mid];")
    c.append("        }")
    c.append("        if (id < can_id) {")
    c.append("            lo = mid + 1;")
    c.append("        } else {")
    c.append("            hi = mid;")
    c.append("        }")
    c.append("    }")
    c.append("    return NULL;")
    c.append("}")
    c.append("")
    c.append("const s2_message_def_t *s2_dbc_message_of_signal(uint16_t signal_index)")
    c.append("{")
    c.append("    for (size_t i = 0; i < s2_dbc_message_count; i++) {")
    c.append("        const s2_message_def_t *m = &s2_dbc_messages[i];")
    c.append("        if (signal_index >= m->first_signal && signal_index < (uint16_t)(m->first_signal + m->signal_count)) {")
    c.append("            return m;")
    c.append("        }")
    c.append("    }")
    c.append("    return NULL;")
    c.append("}")
    c.append("")
    c.append("const s2_signal_def_t *s2_dbc_signal(uint16_t signal_index)")
    c.append("{")
    c.append("    const s2_message_def_t *m = s2_dbc_message_of_signal(signal_index);")
    c.append("    return m ? &m->signals[signal_index - m->first_signal] : NULL;")
    c.append("}")
    c.append("")
    return "\n".join(h) + "\n", "\n".join(c) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dbc", default=DEFAULT_DBC)
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    messages, protected, unprotected, text = parse_dbc(args.dbc)
    header, source = generate(messages, protected, unprotected, text, args.dbc)
    outputs = {
        os.path.join(args.out_dir, "s2_dbc_gen.h"): header,
        os.path.join(args.out_dir, "s2_dbc_gen.c"): source,
    }
    if args.check:
        stale = [p for p, content in outputs.items()
                 if not os.path.exists(p) or open(p, encoding="utf-8").read() != content]
        if stale:
            print("stale generated files: " + ", ".join(os.path.relpath(p, ROOT) for p in stale))
            return 1
        print("generated files are up to date")
        return 0
    os.makedirs(args.out_dir, exist_ok=True)
    for path, content in outputs.items():
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
    n_sig = sum(len(m.signals) for m in messages)
    e2e_in_dbc = sorted(i for i in protected if i in {m.id for m in messages})
    print("%d messages, %d signals -> %s" % (len(messages), n_sig, os.path.relpath(args.out_dir, ROOT)))
    print("E2E-protected ids in header: %d (%d present as BO_), unprotected: %d"
          % (len(protected), len(e2e_in_dbc), len(unprotected)))
    missing_bo = sorted((protected | unprotected) - {m.id for m in messages})
    if missing_bo:
        print("note: ids listed in the header without a BO_: " + " ".join("0x%03X" % i for i in missing_bo))
    return 0


if __name__ == "__main__":
    sys.exit(main())
