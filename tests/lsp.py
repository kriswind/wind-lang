"""Drives `wind lsp` over stdio and checks its responses.

Run from the repository root: python3 tests/lsp.py
"""
import json
import os
import subprocess
import sys

URI = "file:///test.wind"
# Line 1 holds an astral character (two UTF-16 units, four bytes) before the
# use of Name, so byte columns and LSP columns differ.
TEXT = 'const string Name = "v\\n"\ncout << "\U0001F600é" << Name << Nope\n'

# "::" imports are looked up in tests/libs.
proc = subprocess.Popen(["build/wind", "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        env=dict(os.environ, WIND_LIBS="tests/libs"))
next_id = 0
failures = 0


def send(payload):
    body = json.dumps(payload).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n%s" % (len(body), body))
    proc.stdin.flush()


def read():
    length = None
    while True:
        line = proc.stdout.readline()
        if not line:
            sys.exit("FAIL server closed the stream")
        if line == b"\r\n":
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    return json.loads(proc.stdout.read(length))


def notify(method, params):
    send({"jsonrpc": "2.0", "method": method, "params": params})


def request(method, params):
    global next_id
    next_id += 1
    send({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params})
    reply = read()
    check(method + " id", reply.get("id"), next_id)
    return reply


def check(what, actual, expected):
    global failures
    if actual != expected:
        failures += 1
        print("FAIL %s\n  expected %r\n  actual   %r" % (what, expected, actual))


def rng(line, start, end):
    return {"start": {"line": line, "character": start}, "end": {"line": line, "character": end}}


def at(line, character):
    return {"textDocument": {"uri": URI}, "position": {"line": line, "character": character}}


caps = request("initialize", {"capabilities": {}})["result"]["capabilities"]
check("sync", caps["textDocumentSync"], {"openClose": True, "change": 1, "save": True})
check("completion triggers", caps["completionProvider"], {"triggerCharacters": [".", '"', "/", ":"]})
notify("initialized", {})

notify("textDocument/didOpen", {"textDocument": {"uri": URI, "languageId": "wind", "version": 1, "text": TEXT}})
diags = read()
check("publish method", diags["method"], "textDocument/publishDiagnostics")
check("publish uri", diags["params"]["uri"], URI)
check("diagnostics", diags["params"]["diagnostics"], [
    {"range": rng(1, 25, 29), "severity": 1, "source": "wind", "message": "undefined name 'Nope'"},
])

hover = request("textDocument/hover", at(1, 18))["result"]
check("hover text", hover["contents"]["value"], '```wind\nconst string Name = "v\\n"\n```')
check("hover range", hover["range"], rng(1, 17, 21))
check("hover on nothing", request("textDocument/hover", at(1, 2))["result"], None)

check("definition", request("textDocument/definition", at(1, 21))["result"],
      {"uri": URI, "range": rng(0, 13, 17)})

labels = [item["label"] for item in request("textDocument/completion", at(1, 5))["result"]]
check("completion has keywords", [k for k in ("const", "if", "goto", "u128") if k not in labels], [])
check("completion has constant", "Name" in labels, True)
labels = [item["label"] for item in request("textDocument/completion", at(0, 0))["result"]]
check("completion above declaration", "Name" in labels, False)

notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 2},
                                  "contentChanges": [{"text": 'const string Unused = "x"\n'}]})
check("diagnostics after change", read()["params"]["diagnostics"], [
    {"range": rng(0, 13, 19), "severity": 2, "source": "wind", "message": "constant 'Unused' is never used"},
])

FUNCS = """const int Limit = 3
int twice(int n) {
    int doubled = n * 2
    return doubled
}
int total = twice(Limit)
again:
cout << total
goto again
"""
notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 3}, "contentChanges": [{"text": FUNCS}]})
check("diagnostics for valid program", read()["params"]["diagnostics"], [])


def hover_text(line, character):
    return request("textDocument/hover", at(line, character))["result"]["contents"]["value"]


check("hover function call", hover_text(5, 13), "```wind\nint twice(int n)\n```")
check("hover parameter", hover_text(2, 18), "```wind\nint n\n```")
check("hover variable", hover_text(7, 9), "```wind\nint total\n```")
check("hover constant", hover_text(5, 19), "```wind\nconst int Limit = 3\n```")
check("hover label", hover_text(8, 6), "```wind\nagain:\n```")
check("definition of label", request("textDocument/definition", at(8, 6))["result"]["range"], rng(6, 0, 5))
check("definition of function", request("textDocument/definition", at(5, 13))["result"]["range"], rng(1, 4, 9))

inside = [item["label"] for item in request("textDocument/completion", at(3, 4))["result"]]
check("completion inside function", [n in inside for n in ("n", "doubled", "Limit", "twice", "total", "again")],
      [True, True, True, True, False, False])
outside = [item["label"] for item in request("textDocument/completion", at(8, 0))["result"]]
check("completion at top level", [n in outside for n in ("n", "doubled", "Limit", "twice", "total")],
      [False, False, True, True, True])

notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 4},
                                  "contentChanges": [{"text": "u8 level\ncin >> level >> f32 ratio\ncout << level << cin << ratio\n"}]})
check("diagnostics for cin", read()["params"]["diagnostics"], [])
check("hover cin target", hover_text(1, 8), "```wind\nu8 level\n```")
check("hover cin declaration", hover_text(1, 22), "```wind\nf32 ratio\n```")
check("definition of cin declaration", request("textDocument/definition", at(2, 27))["result"]["range"], rng(1, 20, 25))
check("definition of cin target", request("textDocument/definition", at(1, 8))["result"]["range"], rng(0, 3, 8))

ARRAYS = "int nums[3] = {1, 2, 3}\nint first(int values[]) {\n    return values[0]\n}\ncout << first(nums) << nums[1]\n"
notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 5}, "contentChanges": [{"text": ARRAYS}]})
check("diagnostics for arrays", read()["params"]["diagnostics"], [])
check("hover array", hover_text(4, 16), "```wind\nint nums[3]\n```")
check("hover array parameter", hover_text(2, 12), "```wind\nint values[]\n```")
check("hover function with array parameter", hover_text(4, 9), "```wind\nint first(int values[])\n```")
check("definition of indexed array", request("textDocument/definition", at(4, 24))["result"]["range"], rng(0, 4, 8))

LOOPS = "int total = 0\nfor (int step = 0; step < 3; step++) {\n    total += step\n}\ncout << total\n"
notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 6}, "contentChanges": [{"text": LOOPS}]})
check("diagnostics for loops", read()["params"]["diagnostics"], [])
check("hover loop variable", hover_text(2, 15), "```wind\nint step\n```")
in_loop = [item["label"] for item in request("textDocument/completion", at(2, 4))["result"]]
after_loop = [item["label"] for item in request("textDocument/completion", at(4, 0))["result"]]
check("loop variable is scoped to the loop", ["step" in in_loop, "step" in after_loop, "while" in in_loop], [True, False, True])

DEAD = "int f(int n) {\n    return n\n    cout << \"a\"\n    cout << \"b\"\n}\ncout << f(1)\nwhile false { cout << 1 }\n"
notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 7}, "contentChanges": [{"text": DEAD}]})
check("unreachable code is tagged and spans lines", read()["params"]["diagnostics"], [
    {"range": {"start": {"line": 2, "character": 4}, "end": {"line": 3, "character": 15}}, "severity": 2,
     "source": "wind", "message": "unreachable code: it follows a 'return'", "tags": [1]},
    {"range": rng(6, 14, 23), "severity": 2, "source": "wind",
     "message": "unreachable code: the loop's condition is always false", "tags": [1]},
])

# A document that imports a file on disk. The document itself is not on
# disk; its URI only says where its imports are looked up.
MODULES = os.path.abspath("tests/ok/modules")
UTIL_URI = "file://" + MODULES + "/util.wind"
MAIN_URI = "file://" + MODULES + "/unsaved%20main.wind"
MAIN = 'import "util.wind" as u\ncout << u.scaled(u.SCALE) << u.calls\ncout << u.\n'


def at_main(line, character):
    return {"textDocument": {"uri": MAIN_URI}, "position": {"line": line, "character": character}}


notify("textDocument/didOpen", {"textDocument": {"uri": MAIN_URI, "languageId": "wind", "version": 1, "text": MAIN}})
check("diagnostics with an import", [d["message"] for d in read()["params"]["diagnostics"]], [
    "'calls' is private to module 'u'; mark it 'export' there to use it here",
    "expected a name, found end of line",
])
hover = request("textDocument/hover", at_main(1, 12))["result"]
check("hover imported function", hover["contents"]["value"], "```wind\nexport int scaled(int n)\n```")
hover = request("textDocument/hover", at_main(1, 20))["result"]
check("hover imported constant", hover["contents"]["value"], "```wind\nexport const int SCALE = 10\n```")
hover = request("textDocument/hover", at_main(1, 8))["result"]
check("hover module", hover["contents"]["value"], '```wind\nimport "util.wind" as u\n```')
check("definition in imported file", request("textDocument/definition", at_main(1, 12))["result"],
      {"uri": UTIL_URI, "range": rng(6, 11, 17)})
check("definition of module", request("textDocument/definition", at_main(1, 8))["result"],
      {"uri": UTIL_URI, "range": rng(0, 0, 0)})
check("definition of module name", request("textDocument/definition", at_main(0, 22))["result"],
      {"uri": UTIL_URI, "range": rng(0, 0, 0)})
members = sorted(item["label"] for item in request("textDocument/completion", at_main(2, 10))["result"])
check("completion after a module", members, ["SCALE", "UNIT", "call_count", "scaled"])
names = [item["label"] for item in request("textDocument/completion", at_main(1, 0))["result"]]
check("completion offers the module", "u" in names, True)
notify("textDocument/didSave", {"textDocument": {"uri": MAIN_URI}})
check("save re-analyzes the first document", read()["params"]["uri"], URI)
check("save re-analyzes the second document", len(read()["params"]["diagnostics"]), 2)
notify("textDocument/didChange", {"textDocument": {"uri": MAIN_URI, "version": 2},
                                  "contentChanges": [{"text": 'import "ut\nimport "../mod\ncout << "import "\n'}]})
read()
items = request("textDocument/completion", at_main(0, 10))["result"]
check("import completion lists the directory's files", sorted(item["label"] for item in items),
      ["my-counter.wind", "shapes.wind", "util.wind"])
check("import completion replaces what was typed", [item["textEdit"] for item in items if item["label"] == "util.wind"],
      [{"range": rng(0, 8, 10), "newText": "util.wind"}])
items = request("textDocument/completion", at_main(1, 14))["result"]
check("import completion gives whole paths through directories",
      [n in [item["label"] for item in items]
       for n in ("../modules/util.wind", "../hello.wind", "../modules/", "../hello.out", "../modules/unsaved main.wind")],
      [True, True, False, False, False])
check("import completion replaces the whole path",
      [item["textEdit"]["range"] for item in items if item["label"] == "../modules/util.wind"], [rng(1, 8, 14)])
other = [item["label"] for item in request("textDocument/completion", at_main(2, 16))["result"]]
check("no path completion in other strings", "util.wind" in other, False)

notify("textDocument/didChange", {"textDocument": {"uri": MAIN_URI, "version": 3},
                                  "contentChanges": [{"text": "import sh\nimport ::num\nimport ::numbers/r\n"
                                                              "import :\nint x = true ? 1 :\n"
                                                              "import util as u\nextern \"stdio.h\" const i32 EOF\n"
                                                              "cout << u.SCALE << EOF\n"}]})
read()
items = request("textDocument/completion", at_main(0, 9))["result"]
check("import completion without quotes leaves off .wind",
      sorted(item["label"] for item in items), ["my-counter", "shapes", "util"])
check("import completion without quotes replaces what was typed",
      [item["textEdit"] for item in items if item["label"] == "shapes"],
      [{"range": rng(0, 7, 9), "newText": "shapes"}])
items = request("textDocument/completion", at_main(1, 12))["result"]
check("import completion in the libraries directory",
      sorted((item["label"], item["kind"]) for item in items),
      [("::geometry", 9), ("::geometry/area", 17), ("::geometry/perimeter", 17), ("::geometry/solids", 9),
       ("::geometry/solids/volume", 17), ("::numbers", 9), ("::numbers/rounding", 17), ("::numbers/stats", 17),
       ("::tally", 17)])
items = request("textDocument/completion", at_main(2, 18))["result"]
check("import completion inside a library folder",
      [(item["label"], item["textEdit"]["range"]) for item in items if item["label"] == "::numbers/rounding"],
      [("::numbers/rounding", rng(2, 7, 18))])
check("nothing after one colon of an import", request("textDocument/completion", at_main(3, 8))["result"], [])
check("nothing after the colon of a conditional", request("textDocument/completion", at_main(4, 18))["result"], [])
hover = request("textDocument/hover", at_main(7, 9))["result"]
check("hover module imported without quotes", hover["contents"]["value"], "```wind\nimport util as u\n```")
hover = request("textDocument/hover", at_main(7, 19))["result"]
check("hover external constant", hover["contents"]["value"], '```wind\nextern "stdio.h" const i32 EOF\n```')
notify("textDocument/didClose", {"textDocument": {"uri": MAIN_URI}})
check("diagnostics after closing the importer", read()["params"]["diagnostics"], [])

TYPES = """enum Color { RED, GREEN }
struct Point { int x; int y }
typedef Point Spot
Spot origin = {0, 0}
Color c = GREEN
cout << origin.x << c
"""
notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 8}, "contentChanges": [{"text": TYPES}]})
check("diagnostics for declared types", read()["params"]["diagnostics"], [])
check("hover struct", hover_text(1, 9), "```wind\nstruct Point { int x; int y }\n```")
check("hover enum", hover_text(4, 2), "```wind\nenum Color { RED, GREEN }\n```")
check("hover enum value", hover_text(4, 12), "```wind\nColor GREEN = 1\n```")
check("hover typedef", hover_text(3, 2), "```wind\ntypedef Point Spot\n```")
check("hover variable of declared type", hover_text(5, 10), "```wind\nPoint origin\n```")
check("definition of type", request("textDocument/definition", at(2, 10))["result"]["range"], rng(1, 7, 12))
check("definition of enum value", request("textDocument/definition", at(4, 12))["result"]["range"], rng(0, 18, 23))
kinds = {item["label"]: item["kind"] for item in request("textDocument/completion", at(5, 0))["result"]}
check("completion kinds for types", [kinds.get(n) for n in ("Point", "Color", "RED", "Spot", "struct", "typeof")],
      [22, 13, 20, 22, 14, 14])

POINTERS = """struct Node { int value; Node *next }
Node tail = { 2, NULL }
Node head = { 1, &tail }
Node *at = &head
int **deep
Node *step(Node *from) { return from->next }
at->next->value = 3
cout << step(at).value << (deep == NULL)
"""
notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 9}, "contentChanges": [{"text": POINTERS}]})
check("diagnostics for pointers", read()["params"]["diagnostics"], [])
check("hover self-referential struct", hover_text(1, 2), "```wind\nstruct Node { int value; Node *next }\n```")
check("hover pointer variable", hover_text(6, 1), "```wind\nNode *at\n```")
check("hover pointer to pointer", hover_text(7, 28), "```wind\nint **deep\n```")
check("hover function returning a pointer", hover_text(7, 10), "```wind\nNode *step(Node *from)\n```")
check("definition through an assignment target", request("textDocument/definition", at(6, 1))["result"]["range"],
      rng(3, 6, 8))
check("definition of a pointer's type", request("textDocument/definition", at(5, 13))["result"]["range"],
      rng(0, 7, 11))
check("NULL is offered", "NULL" in [item["label"] for item in request("textDocument/completion", at(7, 0))["result"]],
      True)

BOARD = "#target arduino\nint big = 40000\nint small = 400\ncout << big + small\n"
notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 10}, "contentChanges": [{"text": BOARD}]})
check("a document is checked for the target its #target names",
      [d["message"] for d in read()["params"]["diagnostics"]], ["integer literal does not fit in 'int'"])
notify("textDocument/didChange", {"textDocument": {"uri": URI, "version": 11},
                                  "contentChanges": [{"text": BOARD.replace("#target arduino", "#target c")}]})
check("and for c when it says so", read()["params"]["diagnostics"], [])

notify("$/setTrace", {"value": "off"})
check("unknown request", request("textDocument/formatting", {})["error"]["code"], -32601)

notify("textDocument/didClose", {"textDocument": {"uri": URI}})
check("diagnostics after close", read()["params"]["diagnostics"], [])

check("shutdown", request("shutdown", None)["result"], None)
notify("exit", None)
check("exit code", proc.wait(timeout=5), 0)

print("lsp: %s" % ("ok" if not failures else "%d failure(s)" % failures))
sys.exit(1 if failures else 0)
