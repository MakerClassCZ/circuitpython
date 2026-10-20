import base64, json, os, shutil, subprocess, sys, time, urllib.request
BIN = sys.argv[1]
T = os.path.dirname(os.path.abspath(__file__))
D = os.path.join(T, "CIRCUITPY")
shutil.rmtree(D, ignore_errors=True)
env = dict(os.environ, SDL_VIDEODRIVER="dummy")

class Run:
    def __init__(self, args, cwd=T):
        self.log = open(os.path.join(T, "out.txt"), "wb")
        self.p = subprocess.Popen([BIN] + args, cwd=cwd, stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT, env=env)
    def text(self):
        return open(os.path.join(T, "out.txt"), "rb").read().decode(errors="replace")
    def wait_for(self, s, timeout=8):
        end = time.time() + timeout
        while time.time() < end:
            if s in self.text():
                return True
            time.sleep(0.1)
        return False
    def stop(self):
        self.p.terminate(); self.p.wait(timeout=5); self.log.close()

def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + detail[-300:]))

# 1 + 2: a new directory gets the default files and runs code.py
r = Run(["--drive", "CIRCUITPY"])
check("default code.py runs", r.wait_for("Hello World!"), r.text())
time.sleep(0.5)
check("default files created", sorted(os.listdir(D)) == ["boot_out.txt", "code.py", "lib", "settings.toml"], str(sorted(os.listdir(D))))
# 3: a host edit reloads
with open(os.path.join(D, "code.py"), "w") as f:
    f.write('print("edited on the host")\n')
check("host edit triggers auto-reload", r.wait_for("edited on the host"), r.text())
# 11: a hidden file does not reload
before = r.text().count("soft reboot")
open(os.path.join(D, ".hidden"), "w").write("x")
time.sleep(2)
check("hidden file ignored", r.text().count("soft reboot") == before, r.text())
r.stop()

# 4 + 8: Python sees the drive read-only, os functions, imports from lib/
os.makedirs(os.path.join(D, "lib", "pkg"), exist_ok=True)
open(os.path.join(D, "lib", "helper.py"), "w").write('VALUE = "imported from lib"\n')
open(os.path.join(D, "lib", "pkg", "__init__.py"), "w").write('NAME = "package"\n')
open(os.path.join(D, "code.py"), "w").write('''import os, storage, helper, pkg
print("cwd", os.getcwd(), "| listdir", sorted(os.listdir("/")))
print("stat code.py size", os.stat("/code.py")[6], "| statvfs ok", os.statvfs("/")[0] > 0)
print(helper.VALUE, pkg.NAME)
try:
    open("/written.txt", "w").write("x")
    print("write allowed")
except OSError as e:
    print("write refused", e.errno)
os.chdir("/lib"); print("chdir cwd", os.getcwd(), sorted(os.listdir()))
print("mount", type(storage.getmount("/")).__name__)
''')
r = Run(["--drive", "CIRCUITPY"])
r.wait_for("mount ", 6)
t = r.text()
check("os.getcwd and listdir", "cwd / | listdir ['.hidden', 'boot_out.txt', 'code.py', 'lib', 'settings.toml']" in t, t)
check("os.stat / statvfs", "stat code.py size" in t and "statvfs ok True" in t, t)
check("import from lib/ (module and package)", "imported from lib package" in t, t)
check("Python write refused (read-only like USB)", "write refused 30" in t, t)
check("chdir into lib", "chdir cwd /lib ['helper.py', 'pkg']" in t, t)
check("getmount is VfsPosix", "mount VfsPosix" in t, t)
r.stop()

# 5: boot.py remount makes Python writable
open(os.path.join(D, "boot.py"), "w").write('import storage\nstorage.remount("/", readonly=False)\n')
open(os.path.join(D, "code.py"), "w").write('open("/written.txt", "w").write("from python")\nprint("python wrote", open("/written.txt").read())\n')
r = Run(["--drive", "CIRCUITPY"])
check("boot.py remount -> Python writes", r.wait_for("python wrote from python"), r.text())
check("file visible on the host", open(os.path.join(D, "written.txt")).read() == "from python")
check("boot_out.txt written", "Board ID:desktop" in open(os.path.join(D, "boot_out.txt")).read())
r.stop()
os.remove(os.path.join(D, "boot.py"))

# 6: settings.toml + web workflow through supervisor_vfs on the directory
open(os.path.join(D, "settings.toml"), "w").write('CIRCUITPY_WEB_API_PASSWORD = "pw"\nCIRCUITPY_WEB_API_PORT = 8080\nGREETING = "from settings.toml"\n')
open(os.path.join(D, "code.py"), "w").write('import os, time\nprint("greeting", os.getenv("GREETING"))\nwhile True:\n    time.sleep(1)\n')
r = Run(["--drive", "CIRCUITPY"])
check("settings.toml read", r.wait_for("greeting from settings.toml"), r.text())
auth = {"Authorization": "Basic " + base64.b64encode(b":pw").decode()}
def req(method, path, data=None, headers=None):
    q = urllib.request.Request("http://127.0.0.1:8080" + path, data=data, method=method, headers={**auth, **(headers or {})})
    with urllib.request.urlopen(q, timeout=5) as resp:
        return resp.status, resp.read()
try:
    for _ in range(30):
        try:
            st, body = req("GET", "/fs/", headers={"Accept": "application/json"}); break
        except OSError:
            time.sleep(0.2)
    names = sorted(f["name"] for f in json.loads(body)["files"])
    check("web workflow lists the directory", "code.py" in names and "lib" in names, str(names))
    st, _ = req("PUT", "/fs/uploaded.txt", data=b"over the web", headers={"X-Timestamp": "1700000000000"})
    check("web workflow upload lands in the directory", st in (201, 204) and open(os.path.join(D, "uploaded.txt")).read() == "over the web", str(st))
    check("upload keeps its timestamp", int(os.stat(os.path.join(D, "uploaded.txt")).st_mtime) == 1700000000)
    st, _ = req("PUT", "/fs/newdir/", headers={"X-Timestamp": "1700000000000"})
    check("web workflow mkdir", os.path.isdir(os.path.join(D, "newdir")), str(st))
    st, _ = req("DELETE", "/fs/uploaded.txt")
    check("web workflow delete", not os.path.exists(os.path.join(D, "uploaded.txt")), str(st))
except Exception as e:
    check("web workflow", False, repr(e))
r.stop()

# 10: reset keeps the drive with a relative path
open(os.path.join(D, "settings.toml"), "w").write("")
open(os.path.join(D, "code.py"), "w").write('import microcontroller, os\nif microcontroller.cpu.reset_reason != microcontroller.ResetReason.SOFTWARE:\n    microcontroller.reset()\nprint("after reset still on", sorted(os.listdir("/"))[:3])\n')
r = Run(["--drive", "CIRCUITPY"])
check("reset re-executes on the same directory", r.wait_for("after reset still on"), r.text())
r.stop()
print("DONE")
