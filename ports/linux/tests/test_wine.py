# Smoke test of the Windows build under Wine: boot, directory drive, auto-reload, REPL,
# VfsPosix, storage.map_file, DNS, Ctrl-C, a reset (a new process on Windows) and the web
# workflow. The program runs under a process-count limit and every leftover is killed by PID,
# because a restart loop on Windows nests processes.
# Usage: WINEPREFIX=<dir> test_wine.py circuitpython.exe [wine-binary]
import base64, json, os, resource, shutil, subprocess, sys, time, urllib.request
EXE = sys.argv[1]
WINE = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("WINE", "wine")
T = os.path.dirname(os.path.abspath(__file__))
D = os.path.join(T, "WINDRIVE")
LOG = os.path.join(T, "wine.txt")
shutil.rmtree(D, ignore_errors=True)
env = dict(os.environ, WINEDEBUG="-all", SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
env.setdefault("WINEPREFIX", os.path.join(T, "wineprefix"))

def limit():
    # Enough for Wine's helpers and a handful of generations, never a fork storm.
    soft = len(os.listdir("/proc")) + 120
    resource.setrlimit(resource.RLIMIT_NPROC, (soft, soft))

def kill_all():
    # Only the program's own processes: Wine runs them with the .exe as the first word. This
    # script's command line names the .exe too, so a substring match would kill the script.
    me = os.getpid()
    out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
    for line in out.splitlines()[1:]:
        pid, _, args = line.strip().partition(" ")
        first = args.split(" ")[0]
        if int(pid) != me and first.lower().endswith("circuitpython.exe"):
            try:
                os.kill(int(pid), 9)
            except (ProcessLookupError, PermissionError):
                pass

def text():
    return open(LOG, "rb").read().decode(errors="replace")

def wait_for(s, timeout, count=1):
    end = time.time() + timeout
    while time.time() < end:
        if text().count(s) >= count:
            return True
        if text().count("hard fault") >= 2:
            return False
        time.sleep(0.2)
    return False

def check(name, ok):
    print(("PASS " if ok else "FAIL ") + name)

def repl(line):
    p.stdin.write(line.encode() + b"\r")
    p.stdin.flush()

log = open(LOG, "wb")
p = subprocess.Popen([WINE, EXE, "--drive", D], cwd=T, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=env, preexec_fn=limit)
try:
    check("boots and runs the default code.py", wait_for("Hello World!", 90))
    time.sleep(1)
    check("default files created", os.path.isdir(D) and sorted(os.listdir(D)) == ["boot_out.txt", "code.py", "lib", "settings.toml"])
    if os.path.isdir(D):
        open(os.path.join(D, "code.py"), "w").write('import os, time\nprint("edited on the host", os.getcwd(), sorted(os.listdir("/")))\nt=time.monotonic(); time.sleep(0.1); print("sleep", round(time.monotonic()-t, 2))\n')
    check("host edit triggers auto-reload", wait_for("edited on the host", 20))
    check("sleep timing", wait_for("sleep 0.1", 10))
    repl("")
    check("REPL prompt", wait_for(">>> ", 15))
    repl("import board; print('board', board.board_id)")
    check("REPL evaluates", wait_for("board desktop", 15))
    repl("import storage; print(type(storage.getmount('/')).__name__)")
    check("VfsPosix mount", wait_for("VfsPosix", 15))
    repl("f = open('/code.py', 'rb'); runs = storage.map_file(f); print('mapped', len(runs), bytes(runs[0][:9]))")
    check("storage.map_file maps a file", wait_for("mapped 1 b'import os'", 15))
    repl("import socketpool, hostnetwork, ssl; pool = socketpool.SocketPool(hostnetwork.HostNetwork()); print('port', pool.getaddrinfo('example.com', 443)[0][4][1])")
    check("DNS through Winsock", wait_for("port 443", 30))
    repl("import time; time.sleep(60)")
    time.sleep(2)
    p.stdin.write(b"\x03"); p.stdin.flush()
    check("Ctrl-C interrupts", wait_for("KeyboardInterrupt", 15))
    # A reset starts a new process; the web workflow comes up from settings.toml.
    open(os.path.join(D, "settings.toml"), "w").write('CIRCUITPY_WEB_API_PASSWORD = "pw"\nCIRCUITPY_WEB_API_PORT = 8080\n')
    open(os.path.join(D, "code.py"), "w").write('import time\nprint("after reset")\nwhile True:\n    time.sleep(1)\n')
    time.sleep(1)
    repl("import microcontroller; microcontroller.reset()")
    check("reset restarts the program", wait_for("after reset", 60))
    auth = {"Authorization": "Basic " + base64.b64encode(b":pw").decode()}
    def req(method, path, data=None, headers=None):
        q = urllib.request.Request("http://127.0.0.1:8080" + path, data=data, method=method, headers={**auth, **(headers or {})})
        with urllib.request.urlopen(q, timeout=10) as resp:
            return resp.status, resp.read()
    ok = False
    for _ in range(40):
        try:
            st, body = req("GET", "/fs/", headers={"Accept": "application/json"})
            ok = "code.py" in [f["name"] for f in json.loads(body)["files"]]
            break
        except Exception:
            time.sleep(0.5)
    check("web workflow lists the directory", ok)
    try:
        st, _ = req("PUT", "/fs/uploaded.txt", data=b"over the web", headers={"X-Timestamp": "1700000000000"})
        check("web workflow upload lands in the directory", open(os.path.join(D, "uploaded.txt")).read() == "over the web")
        check("upload keeps its timestamp", int(os.stat(os.path.join(D, "uploaded.txt")).st_mtime) == 1700000000)
        st, _ = req("DELETE", "/fs/uploaded.txt")
        check("web workflow delete", not os.path.exists(os.path.join(D, "uploaded.txt")))
    except Exception as e:
        check("web workflow upload (%r)" % e, False)
finally:
    p.kill(); p.wait()
    kill_all()
    log.close()
t = text()
print("--- faults ---"); print("\n".join(sorted(set(l.strip() for l in t.splitlines() if "fault" in l or "rror" in l)))[:600])
