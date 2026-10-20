# A Debug Adapter Protocol client that drives circuitpython --debug like an editor would:
# breakpoints (one on the first line of code.py), stack, scopes, locals, evaluate, step into a
# library file, step out, step over, continue, and the end of the program.
# Usage: test_dap.py circuitpython [wine-binary]
import json, os, shutil, socket, subprocess, sys, time
if os.name != "nt":
    import resource
BIN = sys.argv[1]
WINE = sys.argv[2] if len(sys.argv) > 2 else None
T = os.path.dirname(os.path.abspath(__file__))
D = os.path.join(T, "DAPDRIVE")
LOG = os.path.join(T, "dap.txt")
PORT = 5699
shutil.rmtree(D, ignore_errors=True)
os.makedirs(os.path.join(D, "lib"))
open(os.path.join(D, "code.py"), "w").write('''import helper
counter = 0

def compute(x):
    y = x * 2
    z = helper.double(y)
    return z + 1

for i in range(3):
    counter += compute(i)
print("result", counter)
''')
open(os.path.join(D, "lib", "helper.py"), "w").write('''def double(v):
    w = v + v
    return w
''')

def limit():
    # On Linux: enough for Wine's helpers and a handful of generations, never a fork storm.
    soft = len(os.listdir("/proc")) + 120
    resource.setrlimit(resource.RLIMIT_NPROC, (soft, soft))

env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", WINEDEBUG="-all")
cmd = ([WINE] if WINE else []) + [BIN, "--drive", D, "--debug", str(PORT)]
log = open(LOG, "wb")
p = subprocess.Popen(cmd, cwd=T, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, env=env,
                     preexec_fn=limit if os.name != "nt" else None)

def text():
    return open(LOG, "rb").read().decode(errors="replace")

def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)[:300]))

class Client:
    def __init__(self):
        self.seq = 1
        self.buf = b""
        self.events = []
        self.sock = None
    def connect(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)
                self.sock.settimeout(20)
                return True
            except OSError:
                time.sleep(0.3)
        return False
    def send(self, command, arguments=None):
        msg = {"seq": self.seq, "type": "request", "command": command, "arguments": arguments or {}}
        self.seq += 1
        body = json.dumps(msg).encode()
        self.sock.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        return msg["seq"]
    def read(self):
        while True:
            head = self.buf.find(b"\r\n\r\n")
            if head >= 0:
                length = int([l for l in self.buf[:head].decode().split("\r\n") if l.lower().startswith("content-length")][0].split(":")[1])
                if len(self.buf) >= head + 4 + length:
                    body = self.buf[head + 4:head + 4 + length]
                    self.buf = self.buf[head + 4 + length:]
                    return json.loads(body)
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("adapter closed the connection")
            self.buf += chunk
    def request(self, command, arguments=None):
        seq = self.send(command, arguments)
        while True:
            msg = self.read()
            if msg["type"] == "event":
                self.events.append(msg)
            elif msg["type"] == "response" and msg["request_seq"] == seq:
                return msg
    def wait_event(self, name, timeout=20):
        for e in list(self.events):
            if e["event"] == name:
                self.events.remove(e)
                return e
        end = time.time() + timeout
        while time.time() < end:
            msg = self.read()
            if msg["type"] == "event":
                if msg["event"] == name:
                    return msg
                self.events.append(msg)
        return None

c = Client()
try:
    end = time.time() + 60
    while time.time() < end and "waiting for the editor" not in text():
        time.sleep(0.3)
    check("program waits for the editor", "waiting for the editor" in text(), text()[-300:])
    check("connects", c.connect(20))
    r = c.request("initialize", {"clientID": "test", "adapterID": "circuitpython"})
    check("initialize", r["success"] and r["body"].get("supportsConfigurationDoneRequest"))
    check("initialized event", c.wait_event("initialized") is not None)
    r = c.request("setBreakpoints", {"source": {"path": os.path.join(D, "code.py")}, "breakpoints": [{"line": 1}, {"line": 6}]})
    check("breakpoints accepted", [b["line"] for b in r["body"]["breakpoints"]] == [1, 6])
    c.request("setExceptionBreakpoints", {"filters": []})
    c.request("configurationDone")
    e = c.wait_event("stopped")
    check("stops on the first line of code.py", e is not None and e["body"]["reason"] == "breakpoint", e)
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("module frame at line 1", st and st[0]["name"] == "<module>" and st[0]["line"] == 1 and st[0]["source"]["path"].replace("\\", "/").endswith("/code.py"), st)
    c.request("continue", {"threadId": 1})
    e = c.wait_event("stopped")
    check("stops at the breakpoint in compute", e is not None and e["body"]["reason"] == "breakpoint", e)
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("stack: compute line 6 over <module> line 10", len(st) == 2 and st[0]["name"] == "compute" and st[0]["line"] == 6 and st[1]["name"] == "<module>" and st[1]["line"] == 10, st)
    scopes = c.request("scopes", {"frameId": 1})["body"]["scopes"]
    locals_ref = [s for s in scopes if s["name"] == "Locals"][0]["variablesReference"]
    globals_ref = [s for s in scopes if s["name"] == "Globals"][0]["variablesReference"]
    v = {x["name"]: x["value"] for x in c.request("variables", {"variablesReference": locals_ref})["body"]["variables"]}
    check("locals x=0 y=0", v.get("x") == "0" and v.get("y") == "0", v)
    g = {x["name"]: x["value"] for x in c.request("variables", {"variablesReference": globals_ref})["body"]["variables"]}
    check("globals counter=0 and i=0", g.get("counter") == "0" and g.get("i") == "0", g)
    r = c.request("evaluate", {"expression": "y + 40", "frameId": 1, "context": "watch"})
    check("evaluate in the frame", r["success"] and r["body"]["result"] == "40", r)
    c.request("stepIn", {"threadId": 1})
    e = c.wait_event("stopped")
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("step in lands in lib/helper.py double()", e["body"]["reason"] == "step" and st[0]["name"] == "double" and st[0]["source"]["path"].replace("\\", "/").endswith("/lib/helper.py"), st)
    c.request("stepOut", {"threadId": 1})
    e = c.wait_event("stopped")
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("step out returns to compute line 7", st[0]["name"] == "compute" and st[0]["line"] == 7, st)
    c.request("next", {"threadId": 1})
    e = c.wait_event("stopped")
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("step over the return reaches <module>", st[0]["name"] == "<module>" and st[0]["line"] in (9, 10), st)
    c.request("continue", {"threadId": 1})
    e = c.wait_event("stopped")
    scopes = c.request("scopes", {"frameId": 1})["body"]["scopes"]
    v = {x["name"]: x["value"] for x in c.request("variables", {"variablesReference": scopes[0]["variablesReference"]})["body"]["variables"]}
    check("second hit: x=1 y=2", v.get("x") == "1" and v.get("y") == "2", v)
    c.request("setBreakpoints", {"source": {"path": os.path.join(D, "code.py")}, "breakpoints": []})
    c.request("continue", {"threadId": 1})
    e = c.wait_event("terminated", 30)
    check("program ends with terminated event", e is not None)
    # Stop in the editor: the program reloads and waits for the editor again.
    end = time.time() + 30
    while time.time() < end and text().count("waiting for the editor") < 2:
        time.sleep(0.3)
    c2 = Client()
    check("after the end the program is not waiting (code.py finished)", text().count("waiting for the editor") == 1)
    end = time.time() + 10
    while time.time() < end and "result 15" not in text():
        time.sleep(0.2)
    check("program output result 15", "result 15" in text(), text()[-200:])
except Exception as ex:
    check("protocol run (%r)" % ex, False)
finally:
    p.kill(); p.wait(); log.close()
    if WINE:
        out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
        for line in out.splitlines()[1:]:
            pid, _, args = line.strip().partition(" ")
            if int(pid) != os.getpid() and args.split(" ")[0].lower().endswith("circuitpython.exe"):
                try:
                    os.kill(int(pid), 9)
                except OSError:
                    pass
print("--- tail ---"); print(text().replace("\x1b", "^[")[-500:])
