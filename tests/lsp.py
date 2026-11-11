"""Drives `wind lsp` over stdio and checks its responses.

Run from the repository root: python3 tests/lsp.py
"""
import json
import subprocess
import sys

URI = "file:///test.wind"
# Line 1 holds an astral character (two UTF-16 units, four bytes) before the
# use of Name, so byte columns and LSP columns differ.
TEXT = 'const string Name = "v\\n"\ncout << "\U0001F600é" << Name << Nope\n'

proc = subprocess.Popen(["build/wind", "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
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
check("sync", caps["textDocumentSync"], {"openClose": True, "change": 1})
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

notify("$/setTrace", {"value": "off"})
check("unknown request", request("textDocument/formatting", {})["error"]["code"], -32601)

notify("textDocument/didClose", {"textDocument": {"uri": URI}})
check("diagnostics after close", read()["params"]["diagnostics"], [])

check("shutdown", request("shutdown", None)["result"], None)
notify("exit", None)
check("exit code", proc.wait(timeout=5), 0)

print("lsp: %s" % ("ok" if not failures else "%d failure(s)" % failures))
sys.exit(1 if failures else 0)
