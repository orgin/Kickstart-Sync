#!/usr/bin/env python3
"""
hw2lua.py - translates the Hollywood subset used by kicksync.hws and
KickSyncCLI.hws into Lua 5.4, so the scripts can be tested without
Hollywood (hwshim.lua implements the Hollywood functions they call).

usage: hw2lua.py script.hws > script.lua

Not a general Hollywood compiler: it knows the statements, operators and
preprocessor commands these scripts use, and fails loudly on anything else.
"""
import os
import re
import sys

KEYWORDS = {
    "function", "endfunction", "if", "then", "elseif", "else", "endif",
    "while", "wend", "for", "to", "step", "next", "repeat", "until",
    "forever", "switch", "case", "default", "endswitch", "return", "break",
    "local", "and", "or", "not", "nil", "true", "false", "const", "end",
}

TOKEN_RE = re.compile(r"""
    (?P<ws>[ \t\r]+)
  | (?P<nl>\n)
  | (?P<bcomment>/\*.*?\*/)
  | (?P<lcomment>;[^\n]*)
  | (?P<lstr>\[\[.*?\]\])
  | (?P<str>"(?:\\.|[^"\\\n])*")
  | (?P<hex>\$[0-9A-Fa-f]+)
  | (?P<num>\d+(?:\.\d+)?)
  | (?P<const>\#[A-Za-z_][A-Za-z0-9_]*)
  | (?P<pre>@[A-Za-z]+)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*\$?)
  | (?P<op>\.\.|<>|<=|>=|<<|>>|==|[-+*/\\%^&|~<>=(){}\[\],.:?\#])
""", re.S | re.X)


def tokenize(src, path):
    toks, pos, line = [], 0, 1
    while pos < len(src):
        m = TOKEN_RE.match(src, pos)
        if not m:
            sys.exit("%s:%d: cannot tokenize near %r" % (path, line, src[pos:pos + 20]))
        kind, text = m.lastgroup, m.group()
        if kind == "nl":
            toks.append(("nl", "\n", line))
        elif kind == "bcomment":
            # keep line numbers right
            for _ in range(text.count("\n")):
                toks.append(("nl", "\n", line))
        elif kind not in ("ws", "lcomment"):
            toks.append((kind, text, line))
        line += text.count("\n")
        pos = m.end()
    toks.append(("nl", "\n", line))
    return toks


def ident(text):
    t = text.lower()
    if t.endswith("$"):
        t = t[:-1] + "_S"
    return t


def logical_lines(toks):
    """Joins physical lines while brackets are open or a line ends with an
    operator that needs a right operand."""
    lines, cur, depth = [], [], 0
    cont_ops = {"..", ",", "+", "-", "*", "/", "and", "or", "=", "<>", "(", "{", "["}
    for t in toks:
        if t[0] == "nl":
            last = cur[-1] if cur else None
            lastv = last[1].lower() if last else ""
            if depth > 0 or (last and last[0] in ("op", "ident") and lastv in cont_ops):
                continue
            if cur:
                lines.append(cur)
            cur = []
            continue
        if t[1] in ("(", "{", "["):
            depth += 1
        elif t[1] in (")", "}", "]"):
            depth -= 1
        cur.append(t)
    return lines


class Translator:
    def __init__(self, path):
        self.path = path
        self.out = []
        self.switch = []      # stack: has a Case been seen?

    def fail(self, line, msg):
        sys.exit("%s:%d: %s" % (self.path, line, msg))

    def expr(self, toks, assign_ok=False):
        """Translates an expression. '=' is a comparison except directly
        inside a table constructor."""
        res, stack, i = [], [], 0
        while i < len(toks):
            kind, text, line = toks[i]
            low = text.lower()
            if kind == "str" or kind == "lstr":
                res.append(text if kind == "str" else text)
            elif kind == "hex":
                res.append("0x" + text[1:])
            elif kind == "num":
                res.append(text)
            elif kind == "const":
                res.append("C_" + text[1:].lower())
            elif kind == "ident":
                if low in ("and", "or", "not", "nil", "true", "false"):
                    res.append({"and": " and ", "or": " or ", "not": " not ",
                                "nil": "nil", "true": "true", "false": "false"}[low])
                elif low == "function":
                    self.fail(line, "anonymous functions are not supported")
                elif low in KEYWORDS:
                    self.fail(line, "unexpected keyword %s" % text)
                else:
                    res.append(ident(text))
            elif kind == "op":
                if text == "?":
                    # ?Func(args) -> hwtry(func, args)
                    if i + 2 >= len(toks) or toks[i + 1][0] != "ident" or toks[i + 2][1] != "(":
                        self.fail(line, "? must be followed by a function call")
                    fname = ident(toks[i + 1][1])
                    if toks[i + 3][1] == ")":
                        res.append("hwtry(%s" % fname)
                    else:
                        res.append("hwtry(%s, " % fname)
                    stack.append("(")
                    i += 3
                    continue
                if text in ("(", "["):
                    stack.append(text)
                    res.append(text)
                elif text == "{":
                    stack.append(text)
                    res.append("hwt{")
                elif text in (")", "]", "}"):
                    stack.pop()
                    res.append(text)
                elif text == "<>":
                    res.append(" ~= ")
                elif text == "=":
                    if stack and stack[-1] == "{":
                        res.append(" = ")
                    else:
                        res.append(" == ")
                elif text == "\\":
                    res.append(" // ")
                elif text in ("..", "+", "-", "*", "/", "%", "^", "&", "|", "~", "<", ">", "<=", ">=", "<<", ">>"):
                    res.append(" %s " % text)
                elif text in (",", ".", ":"):
                    res.append(text if text != "," else ", ")
                else:
                    self.fail(line, "unsupported operator %s" % text)
            else:
                self.fail(line, "unexpected %s %r" % (kind, text))
            i += 1
        return "".join(res)

    def split_top(self, toks, word):
        """Index of the first top-level token whose lower text is word."""
        depth = 0
        for i, t in enumerate(toks):
            if depth == 0 and t[1].lower() == word and t[0] != "str":
                return i
            if t[1] in ("(", "{", "["):
                depth += 1
            elif t[1] in (")", "}", "]"):
                depth -= 1
        return -1

    def statement(self, toks):
        """A single statement (no block keywords)."""
        kind, text, line = toks[0]
        low = text.lower()
        if low == "return":
            if len(toks) == 1:
                return "do return end"
            return "do return %s end" % self.expr(toks[2:-1])
        if low == "break":
            return "break"
        if low == "local":
            eq = self.split_top(toks, "=")
            if eq < 0:
                return "local " + self.expr(toks[1:])
            return "local %s = %s" % (self.expr(toks[1:eq]), self.expr(toks[eq + 1:]))
        eq = self.split_top(toks, "=")
        if eq > 0:
            return "%s = %s" % (self.expr(toks[:eq]), self.expr(toks[eq + 1:]))
        return self.expr(toks)

    def line(self, toks):
        kind, text, line = toks[0]
        low = text.lower()
        emit = self.out.append
        if kind == "pre":
            if low == "@include":
                inc = os.path.join(os.path.dirname(self.path), toks[1][1][1:-1])
                sub = Translator(inc)
                sub.run(open(inc, encoding="latin-1").read())
                emit("-- @INCLUDE %s" % inc)
                self.out.extend(sub.out)
            return                                  # other preprocessor lines: ignored
        if low == "const":
            emit("%s = %s" % ("C_" + toks[1][1][1:].lower(), self.expr(toks[3:])))
        elif low == "function":
            p = self.split_top(toks, "(")
            emit("function %s(%s)" % (self.expr(toks[1:p]), self.expr(toks[p + 1:-1])))
        elif low in ("endfunction", "endif", "wend"):
            emit("end")
        elif low == "if":
            th = self.split_top(toks, "then")
            if th < 0:
                emit("if hwtrue(%s) then" % self.expr(toks[1:]))
            else:
                if self.split_top(toks[th + 1:], "else") >= 0 or self.split_top(toks[th + 1:], "elseif") >= 0:
                    self.fail(line, "short If with Else is not supported")
                emit("if hwtrue(%s) then %s end" % (self.expr(toks[1:th]), self.statement(toks[th + 1:])))
        elif low == "elseif":
            emit("elseif hwtrue(%s) then" % self.expr(toks[1:]))
        elif low == "else":
            emit("else")
        elif low == "while":
            emit("while hwtrue(%s) do" % self.expr(toks[1:]))
        elif low == "for":
            rest = toks[1:]
            if rest[0][1].lower() == "local":
                rest = rest[1:]
            to = self.split_top(rest, "to")
            st = self.split_top(rest, "step")
            var = ident(rest[0][1])
            if rest[1][1] != "=":
                self.fail(line, "unsupported For")
            start = self.expr(rest[2:to])
            stop = self.expr(rest[to + 1:st if st >= 0 else len(rest)])
            stp = (", " + self.expr(rest[st + 1:])) if st >= 0 else ""
            emit("for %s = %s, %s%s do" % (var, start, stop, stp))
        elif low == "end" and len(toks) == 1:
            emit("hwend()")
        elif low == "next":
            emit("end")
        elif low == "repeat":
            emit("repeat")
        elif low == "until":
            emit("until hwtrue(%s)" % self.expr(toks[1:]))
        elif low == "forever":
            emit("until false")
        elif low == "switch":
            emit("do local hw_sw = %s" % self.expr(toks[1:]))
            self.switch.append(False)
        elif low == "case":
            val = toks[1:-1] if toks[-1][1] == ":" else toks[1:]
            emit("%s hw_sw == %s then" % ("elseif" if self.switch[-1] else "if", self.expr(val)))
            self.switch[-1] = True
        elif low == "default":
            emit("else")
        elif low == "endswitch":
            emit("end end" if self.switch.pop() else "end")
        else:
            emit(self.statement(toks))

    def run(self, src):
        for ln in logical_lines(tokenize(src, self.path)):
            self.line(ln)


def main():
    t = Translator(sys.argv[1])
    t.run(open(sys.argv[1], encoding="latin-1").read())
    print("\n".join(t.out))


if __name__ == "__main__":
    main()
