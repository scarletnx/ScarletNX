#!/usr/bin/env python3
"""Candidates for the code quality rules of RFC #142, for a human to judge. It never blocks anything.

    check.py [PATH...]     (files or folders; default src/; .c and .h)

It prints one line per candidate, file:line: rule: what it saw. Five rules can be seen in the text:
  flag    a presence flag beside the pointer it describes (has_X / X_present / X_valid beside a pointer X,
          or is_present / present / valid in a struct with one pointer): NULL already says it
  allocs  one function filling two or more members of one object from separate allocations: one block
          carved by offsets frees once and keeps them together
  atomics two or more atomics in one struct with no padding array (or alignas) between them, or two defined at
          file scope in one file: they may share a cache line, and two threads writing them slow each other
          down. An atomic is declared _Atomic, or reached through an __atomic_* builtin in the struct's own file
          or in a file read that includes it
  globals two or more variables of static duration in one file (at file scope, or static in a function) that
          aren't const: one struct holds the file's state, in one global. Under src/nx/hle/, any one: the
          state is a member of g_hle (hle/hle.h)
  zero    a local zeroed by memset(&x, 0, sizeof x) in the function that declares it: `= {0}` says
          so where it is declared. A memset stays for an object reached through a pointer
Exit status 0 whatever it finds; 2 when a path cannot be read.
"""
import os, re, sys

ALLOC = r"(?:calloc|malloc|realloc|aligned_alloc|snx_alloc|snx_realloc|SDL_calloc|SDL_malloc)"
ATOMIC = re.compile(r"\b(_Atomic\b|atomic_[a-z_]+\b|SDL_Atomic(?:Int|U32)\b)")
GENERIC_FLAGS = {"is_present", "present", "valid", "is_valid", "has_value"}
BUILTIN = re.compile(r"\b__atomic_\w+\s*\(\s*&?\s*([^,()]+?)\s*[,)]")
ALIGNED = re.compile(r"\b(?:alignas|_Alignas)\s*\(")


def builtin_names(text):
    """The variables and members an __atomic_* builtin reaches: the last name of its first argument."""
    names = set()
    for m in BUILTIN.finditer(text):
        ids = re.findall(r"[A-Za-z_]\w*", re.sub(r"\[[^\]]*\]", "", m.group(1)))
        if ids:
            names.add(ids[-1])
    return names


def strip(text):
    """Comments and string or char literals blanked, newlines kept, so line numbers still match."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', blank, text, flags=re.S)


def closing(text, start):
    """The index of the brace closing the one at start."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if not depth:
                return i
    return len(text) - 1


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def members(body, offset):
    """(name, declaration, index) of each member at the top level of a struct body."""
    out, depth, start = [], 0, 0
    for i, c in enumerate(body):
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif c == ";" and depth == 0:
            decl = body[start:i].strip()
            names = re.findall(r"(\w+)\s*(?:\[[^\]]*\]\s*)*(?::\s*\d+\s*)?(?=,|$)", decl)
            lead = len(body[start:i]) - len(body[start:i].lstrip())
            for name in names:
                out.append((name, decl, offset + start + lead))
            start = i + 1
    return out


def atomic(name, decl, used):
    """Declared _Atomic, or one variable an __atomic_* builtin reaches (an array's elements and a function
    pointer's parameters are not checked)."""
    return bool(ATOMIC.search(decl[:decl.rfind(name)])) or (name in used and "[" not in decl and "(" not in decl)


def check_structs(path, text, report, used):
    for m in re.finditer(r"\b(?:struct|union)\s*\w*\s*\{", text):
        open_at = m.end() - 1
        body = text[open_at + 1:closing(text, open_at)]
        decls = members(body, open_at + 1)
        pointers = {name for name, decl, _ in decls if re.search(r"\*\s*(?:const\s+)?" + re.escape(name) + r"\b", decl)}
        for name, decl, at in decls:
            if not re.match(r"(?:bool|_Bool|int|unsigned|u8|uint8_t|char)\b", decl):
                continue
            target = None
            for pattern in (r"has_(\w+)", r"(\w+)_present", r"(\w+)_valid", r"is_(\w+)_set"):
                hit = re.fullmatch(pattern, name)
                if hit and hit.group(1) in pointers:
                    target = hit.group(1)
            if not target and name in GENERIC_FLAGS and len(pointers) == 1:
                target = next(iter(pointers))
            if target:
                report(path, line_of(text, at), "flag", f"'{name}' beside pointer '{target}'")
        where = [k for k, (name, decl, _) in enumerate(decls) if atomic(name, decl, used)]
        bare = [decls[k][0] for j, k in zip(where, where[1:])
                if not ALIGNED.search(decls[k][1]) and not any("[" in d for _, d, _ in decls[j + 1:k])]
        if bare:
            report(path, line_of(text, decls[where[0]][2]), "atomics",
                   f"{len(where)} atomics in one struct ({', '.join(decls[k][0] for k in where)}), "
                   f"nothing keeps {', '.join(bare)} off the one before")


def bodies(text):
    """The index of each function body's opening brace: a brace at file scope right after a ')'."""
    depth, last = 0, ""
    for i, c in enumerate(text):
        if c == "{":
            if depth == 0 and last == ")":
                yield i
            depth += 1
        elif c == "}":
            depth = max(0, depth - 1)
        if not c.isspace():
            last = c


def check_functions(path, text, report):
    for open_at in bodies(text):
        body = text[open_at:closing(text, open_at)]
        owners = {}
        for a in re.finditer(r"(\b\w+(?:\s*(?:->|\.)\s*\w+)*?)\s*(?:->|\.)\s*\w+\s*(?:\[[^\]]*\])?\s*=\s*"
                             r"(?:\([^()]*\)\s*)?" + ALLOC + r"\s*\(", body):
            owner = re.sub(r"\s+", "", a.group(1))
            owners.setdefault(owner, []).append(open_at + a.start())
        for owner, hits in owners.items():
            if len(hits) > 1:
                report(path, line_of(text, hits[0]), "allocs",
                       f"{len(hits)} separate allocations into members of '{owner}'")
        for z in re.finditer(r"\bmemset\s*\(\s*&\s*(\w+)\s*,\s*0\s*,\s*sizeof\s*\(?\s*\1\s*\)?\s*\)", body):
            name = z.group(1)
            # declared in this body before it: a type, then the name (maybe among others) and ; , or =
            if re.search(r"(?:^|[;{}])\s*(?:const\s+)?\w[\w\s\*]*?[\s\*,]" + name + r"\s*(?:\[[^\]]*\]\s*)*[;,=]",
                         body[: z.start()]):
                report(path, line_of(text, open_at + z.start()), "zero",
                       f"'{name}' zeroed by memset where it is declared: '= {{0}}' there")


def const_object(head):
    """The definition's object is itself const (a const after the last *): a table, not state."""
    consts = [m.start() for m in re.finditer(r"\bconst\b", head)]
    return bool(consts) and consts[-1] > head.rfind("*")


def check_globals(path, text, report, used):
    """Atomics defined at file scope: the compiler lays a file's definitions out together."""
    depth, start, found = 0, 0, []
    for i, c in enumerate(text):
        if c == "{":
            depth += 1
        elif c == "}":
            depth = max(0, depth - 1)
            if depth == 0:
                start = i + 1
        elif c == ";" and depth == 0:
            stmt = text[start:i]
            lead = len(stmt) - len(stmt.lstrip())
            decl = stmt.strip()
            # an extern or a typedef does not match: only definitions take space here
            head = decl.split("=")[0]
            if "(" not in head and not re.match(r"(?:extern|typedef)\b", decl):
                name = (re.findall(r"(\w+)\s*(?:\[[^\]]*\]\s*)*$", head.strip()) or [""])[0]
                bare_decl = re.sub(r"^(?:static|_Thread_local|thread_local|volatile)\s+", "", decl)
                if ATOMIC.match(bare_decl) or (name in used and "[" not in head):
                    found.append((name, decl, start + lead))
            start = i + 1
    if len(found) > 1:
        bare = [name for name, decl, _ in found[1:] if not ALIGNED.search(decl)]
        if bare:
            report(path, line_of(text, found[0][2]), "atomics",
                   f"{len(found)} atomics at file scope ({', '.join(n for n, _, _ in found)}), "
                   f"nothing keeps {', '.join(bare)} off the one before")


def check_state(path, text, report):
    """A file's variables of static duration, at file scope or static in a function, when two or more aren't const;
    and its thread-locals, when there are two or more, or any in a subsystem that keeps one thread-local for all."""
    variables, tls, depth, start, brace = [], [], 0, 0, -1
    for i, c in enumerate(text):
        if c == "{":
            if depth == 0:
                brace = i
            depth += 1
        elif c == "}":
            depth = max(0, depth - 1)
            if depth == 0 and text[start:brace].rstrip().endswith(")"):
                start = i + 1  # a function's body ends its statement
        elif c == ";" and depth == 0:
            stmt = re.sub(r"^[ \t]*#.*$", lambda m: " " * len(m.group(0)), text[start:i], flags=re.M)
            lead = len(stmt) - len(stmt.lstrip())
            decl = stmt.strip()
            while re.search(r"\{[^{}]*\}", decl):
                decl = re.sub(r"\{[^{}]*\}", "", decl)
            head = decl.split("=")[0]
            # a thread's own variables can't be members of one global: rule 6 gathers them in one thread-local
            thread_local = re.match(r"(?:static\s+)?(?:_Thread_local|thread_local|__thread)\b", decl)
            if thread_local and "(" not in head:
                names = re.findall(r"(\w+)\s*(?:\[[^\]]*\]\s*)*$", head.strip())
                if names:
                    tls.append((names[0], start + lead))
            if decl and not thread_local and "(" not in head and not re.match(r"(?:extern|typedef)\b", decl) and \
                    not re.fullmatch(r"(?:struct|union|enum)\b\s*\w*\s*", head.strip()) and not const_object(head):
                for part in head.split(","):
                    names = re.findall(r"(\w+)\s*(?:\[[^\]]*\]\s*)*$", part.strip())
                    if names:
                        variables.append((names[0], start + lead))
            start = i + 1
    for open_at in bodies(text):
        body = text[open_at + 1:closing(text, open_at)]
        while re.search(r"\b(?:struct|union)\s*\w*\s*\{[^{}]*\}", body):  # a static struct's members are its own
            body = re.sub(r"\b(?:struct|union)\s*\w*\s*\{[^{}]*\}", lambda m: "struct" + " " * (len(m.group(0)) - 6),
                          body)
        for m in re.finditer(r"\bstatic\b([^;(]*?)(\w+)\s*(?:\[[^\]]*\]\s*)*(?:=[^;]*)?;", body):
            if not const_object(m.group(1)):
                variables.append((m.group(2), open_at + 1 + m.start()))
    hle = "src/nx/hle/" in path.replace(os.sep, "/") and not path.endswith("hle.c")
    if hle and variables:
        variables.sort(key=lambda v: v[1])
        report(path, line_of(text, variables[0][1]), "globals",
               f"{len(variables)} variables of static duration ({', '.join(n for n, _ in variables)}): HLE state "
               f"is a member of g_hle (hle/hle.h)")
    elif len(variables) > 1:
        variables.sort(key=lambda v: v[1])
        report(path, line_of(text, variables[0][1]), "globals",
               f"{len(variables)} variables of static duration ({', '.join(n for n, _ in variables)}): one struct "
               f"holds a file's state")
    # thread state: one thread-local struct a file, and in a subsystem with one global, one for the subsystem
    unix = path.replace(os.sep, "/")
    slot = "t_hle" if "src/nx/hle/" in unix else "t_nvn" if "src/nx/gpu/nvn/" in unix else None
    loose = [v for v in tls if v[0] != slot] if slot else []
    tls.sort(key=lambda v: v[1])
    if loose:
        report(path, line_of(text, loose[0][1]), "tls",
               f"{len(loose)} thread-local variable{'s' if len(loose) > 1 else ''} "
               f"({', '.join(n for n, _ in loose)}): the subsystem's thread state is a member of {slot}")
    elif len(tls) > 1:
        report(path, line_of(text, tls[0][1]), "tls",
               f"{len(tls)} thread-local variables ({', '.join(n for n, _ in tls)}): one thread-local struct holds "
               f"a file's thread state")


def files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in sorted(names):
                    if n.endswith((".c", ".h")):
                        yield os.path.join(root, n)
        else:
            yield p


def main(argv):
    paths = argv[1:] or ["src"]
    found, failed = [], False

    def report(path, line, rule, what):
        found.append(f"{path}:{line}: {rule}: {what}")

    texts, includes = {}, {}
    for path in files(paths):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                raw = f.read()
            texts[path] = strip(raw)
            includes[path] = {os.path.basename(n) for n in re.findall(r'^\s*#\s*include\s*"([^"]+)"', raw, re.M)}
        except OSError as e:
            print(f"{path}: {e.strerror}", file=sys.stderr)
            failed = True
    reached = {path: builtin_names(text) for path, text in texts.items()}
    by_name = {}
    for path in texts:
        by_name.setdefault(os.path.basename(path), []).append(path)
    seen = {}

    def closure(path):
        """The headers a file includes, and theirs, by name."""
        if path not in seen:
            seen[path], todo = set(), list(includes[path])
            while todo:
                n = todo.pop()
                if n not in seen[path]:
                    seen[path].add(n)
                    todo.extend(i for q in by_name.get(n, ()) for i in includes[q])
        return seen[path]
    for path, text in texts.items():
        # a member's name counts where this file's code, or code including it, reaches it atomically
        name = os.path.basename(path)
        used = set(reached[path]).union(*(reached[p] for p in texts if name in closure(p)))
        check_structs(path, text, report, used)
        check_functions(path, text, report)
        check_globals(path, text, report, used)
        check_state(path, text, report)
    for line in found:
        print(line)
    print(f"{len(found)} candidate(s)", file=sys.stderr)
    return 2 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
