#!/usr/bin/env python3
# ===========================================================================
# m80_to_pyz80.py - convert Frank Cringle's ZEXDOC/ZEXALL M80-style source
# (agn453 .z80) into pyz80 (COMET/SAM) syntax so it can be assembled with the
# pip 'pyz80' assembler. Expands the tstr/tmsg macros, translates directives
# and hex literals, injects the CP/M bdos vector, and drops the if-0 block.
#
#   m80_to_pyz80.py <in.z80> <out.asm> [keep_test1 keep_test2 ...]
#
# If keep_tests are given, the `tests:` pointer table is trimmed to just those
# (used to isolate specific subtests for fast iteration).
# ===========================================================================
import re, sys

HEX = re.compile(r'\b([0-9][0-9A-Fa-f]*)[hH]\b')
def conv(expr):
    """translate an M80 expression to pyz80: char consts, hex literals, low/high"""
    expr = re.sub(r"'(.)'", lambda m: str(ord(m.group(1))), expr)   # 'a' -> 97
    expr = re.sub(r'\blow\s+([A-Za-z_]\w*)',  r'((\1)&0xff)', expr, flags=re.I)
    expr = re.sub(r'\bhigh\s+([A-Za-z_]\w*)', r'(((\1)>>8)&0xff)', expr, flags=re.I)
    return HEX.sub(lambda m: '0x' + m.group(1), expr)

def strip_comment(s):
    """remove a ; comment, respecting single-quoted strings"""
    inq = False
    for i, ch in enumerate(s):
        if ch == "'":
            inq = not inq
        elif ch == ';' and not inq:
            return s[:i]
    return s

def split_args(s):
    """split a macro/directive operand list on commas, respecting <...> and '...'"""
    out, depth, cur, inq = [], 0, '', False
    for ch in s:
        if ch == "'":
            inq = not inq; cur += ch
        elif ch == '<' and not inq:
            depth += 1; cur += ch
        elif ch == '>' and not inq:
            depth -= 1; cur += ch
        elif ch == ',' and depth == 0 and not inq:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip() != '' or out:
        out.append(cur.strip())
    return out

def insn_bytes(arg):
    """first tstr arg: <a,b,c> list or single value -> list of converted byte exprs"""
    arg = arg.strip()
    if arg.startswith('<') and arg.endswith('>'):
        items = [x.strip() for x in arg[1:-1].split(',')]
    else:
        items = [arg]
    return [conv(x) for x in items]

def strip_quotes(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == "'" and s[-1] == "'":
        return s[1:-1]
    return s

def expand_tstr(args, out):
    insn = insn_bytes(args[0])
    rest = args[1:]                       # memop,iy,ix,hl,de,bc,flags,acc,sp
    out.append('  defb ' + ','.join(insn))
    if len(insn) < 4:
        out.append('  defb ' + ','.join(['0'] * (4 - len(insn))))
    memop, iy, ix, hl, de, bc, flags, acc, sp = rest
    out.append('  defw ' + ','.join(conv(x) for x in (memop, iy, ix, hl, de, bc)))
    out.append('  defb ' + conv(flags))
    out.append('  defb ' + conv(acc))
    out.append('  defw ' + conv(sp))

def expand_tmsg(arg, out):
    msg = strip_quotes(arg)
    out.append('  defm "%s"' % msg)
    pad = 30 - len(msg)
    if pad > 0:
        out.append('  defb ' + ','.join(['0x2e'] * pad))   # '.' padding
    out.append('  defb 0x24')                               # '$'

def conv_db(operand, out):
    """db with mixed strings / numbers / char-consts -> defm + defb sequence"""
    nums = []
    def flush():
        if nums:
            out.append('  defb ' + ','.join(nums)); nums.clear()
    for item in split_args(operand):
        it = item.strip()
        if it.startswith("'") and it.endswith("'") and len(it) >= 2:
            content = it[1:-1]
            if len(content) == 1:
                nums.append(str(ord(content)))     # single char -> byte value
            else:
                flush(); out.append('  defm "%s"' % content)
        else:
            nums.append(conv(it))
    flush()

def main():
    inf, outf = sys.argv[1], sys.argv[2]
    keep = sys.argv[3:]                    # tests to keep (trim the table)
    lines = open(inf).read().splitlines()
    out = ['; auto-generated from %s by m80_to_pyz80.py' % inf]
    i, n = 0, len(lines)
    in_macro = False
    in_tests = False
    injected_bdos = False
    while i < n:
        raw = lines[i]; i += 1
        # M80 column-1 labels may omit the trailing ':' (e.g. "bdos  push af")
        if raw[:1] not in (' ', '\t', ';', ''):
            ml = re.match(r'^([A-Za-z_]\w*)(:?)(.*)$', raw)
            if ml and ml.group(2) == '':
                raw = ml.group(1) + ':' + ml.group(3)
        line = raw.rstrip()
        s = line.strip()
        low = s.lower()

        # skip macro definitions
        if re.match(r'^\w+:?\s+macro\b', s, re.I):
            in_macro = True; continue
        if in_macro:
            if low.startswith('endm'):
                in_macro = False
            continue
        # drop "if 0 ... endif" dead block
        if re.match(r'^if\s+0\b', s, re.I):
            depth = 1
            while i < n and depth:
                t = lines[i].strip().lower(); i += 1
                if t.startswith('if'): depth += 1
                elif t.startswith('endif'): depth -= 1
            continue
        # directives to drop
        if low.startswith('.title') or low == 'aseg' or low.startswith('.z80'):
            continue
        # org
        m = re.match(r'^org\s+(.*)', s, re.I)
        if m:
            out.append('org ' + conv(m.group(1)))
            continue
        # tests table trimming
        if re.match(r'^tests:', s):
            in_tests = True
            out.append('tests:')
            if keep:
                for k in keep:
                    out.append('  defw ' + k)
                out.append('  defw 0')
                # skip original table entries until the terminating dw 0
                while i < n:
                    t = lines[i].strip();
                    if re.match(r'^dw\s+0\s*($|;)', t): i += 1; break
                    i += 1
            continue
        # label-only line (e.g. "tests:") handled above; generic label+rest
        # equ:  NAME equ EXPR   /  NAME: equ EXPR
        m = re.match(r'^(\w+):?\s+equ\s+(.*)', s, re.I)
        if m:
            out.append('%s: equ %s' % (m.group(1), conv(m.group(2)))); continue

        # split optional leading label "lab:" from the rest
        lab = ''
        m = re.match(r'^(\w+:)\s*(.*)', s)
        if m:
            lab, s = m.group(1), m.group(2)
        # strip trailing comment for keyword detection (keep for passthrough)
        body = s
        kw = body.split()[0].lower() if body.split() else ''

        if kw == 'tstr':
            if lab: out.append(lab)
            expand_tstr(split_args(strip_comment(body[4:]).strip()), out); continue
        if kw == 'tmsg':
            if lab: out.append(lab)
            expand_tmsg(strip_comment(body[4:]).strip(), out); continue
        if kw == 'db':
            if lab: out.append(lab)
            conv_db(body[2:].strip().split(';')[0], out); continue
        if kw == 'dw':
            if lab: out.append(lab)
            out.append('  defw ' + ','.join(conv(x) for x in split_args(body[2:].split(';')[0].strip()))); continue
        if kw == 'ds':
            if lab: out.append(lab)
            arg = body[2:].split(';')[0].strip()
            out.append('  defs ' + conv(split_args(arg)[0])); continue

        # passthrough (mnemonics / labels): convert the code part only, keep comment
        # pyz80 wants the implicit-A form: and/or/xor/cp/sub take one operand
        s = re.sub(r'^(and|or|xor|cp|sub)\s+a\s*,\s*', r'\1 ', s, flags=re.I)
        code, sep, comment = s.partition(';')
        full = (lab + ' ' if lab else '') + conv(code) + sep + comment
        out.append(full)

    open(outf, 'w').write('\n'.join(out) + '\n')

if __name__ == '__main__':
    main()
