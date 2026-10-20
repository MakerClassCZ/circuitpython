import base64, os, shutil, subprocess, sys, time, urllib.request, urllib.error
BIN = sys.argv[1]; T = os.path.dirname(os.path.abspath(__file__)); D = os.path.join(T, "CP2")
shutil.rmtree(D, ignore_errors=True)
env = dict(os.environ, SDL_VIDEODRIVER="dummy")
LOG = os.path.join(T, "out2.txt")
class Run:
    def __init__(self, stdin=subprocess.DEVNULL):
        self.log = open(LOG, "wb")
        self.p = subprocess.Popen([BIN, "--drive", D], cwd=T, stdin=stdin, stdout=self.log, stderr=subprocess.STDOUT, env=env)
    def text(self): return open(LOG, "rb").read().decode(errors="replace")
    def wait_for(self, s, timeout=8, count=1):
        end = time.time() + timeout
        while time.time() < end:
            if self.text().count(s) >= count: return True
            time.sleep(0.1)
        return False
    def stop(self): self.p.terminate(); self.p.wait(timeout=5); self.log.close()
def check(name, ok, detail=""): print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + detail[-400:].replace("\x1b", "^[")))

r = Run(); r.wait_for("Hello World!")
# a directory made on the host, then a file inside it, then an edit inside it
os.makedirs(os.path.join(D, "assets")); time.sleep(0.2)
n = r.text().count("soft reboot")
open(os.path.join(D, "assets", "a.txt"), "w").write("1")
check("new host subdirectory reloads", r.wait_for("soft reboot", 6, n + 1), r.text())
time.sleep(1.5); n = r.text().count("soft reboot")
open(os.path.join(D, "assets", "a.txt"), "w").write("2")
check("edit inside the new subdirectory reloads", r.wait_for("soft reboot", 6, n + 1), r.text())
time.sleep(1.5); n = r.text().count("soft reboot")
os.rename(os.path.join(D, "assets", "a.txt"), os.path.join(D, "assets", "b.txt"))
check("rename on the host reloads", r.wait_for("soft reboot", 6, n + 1), r.text())
r.stop()

# errors and the REPL
open(os.path.join(D, "code.py"), "w").write('''import os
for op in (lambda: open("/missing.txt"), lambda: os.listdir("/missing"), lambda: os.remove("/code.py")):
    try:
        op()
    except OSError as e:
        print("errno", e.errno)
''')
r = Run(stdin=subprocess.PIPE); r.wait_for("Press any key", 8)
t = r.text()
check("missing file ENOENT, missing dir ENOENT, remove refused EROFS", "errno 2\r\nerrno 2\r\nerrno 30" in t, t)
r.p.stdin.write(b"\r"); r.p.stdin.flush(); r.wait_for(">>> ", 5)
r.p.stdin.write(b"import sys; print('path', sys.path)\r"); r.p.stdin.flush()
check("REPL works on the directory drive", r.wait_for("path ['', '/', '/lib']", 5), r.text())
r.stop()

# remounted for Python: Python writes do not reload, host edits do not either
open(os.path.join(D, "boot.py"), "w").write('import storage\nstorage.remount("/", readonly=False)\n')
open(os.path.join(D, "code.py"), "w").write('import time\nfor i in range(3):\n    open("/log.txt", "a").write("x")\n    time.sleep(0.5)\nprint("python wrote", len(open("/log.txt").read()))\nwhile True:\n    time.sleep(1)\n')
r = Run()
check("Python writes after remount", r.wait_for("python wrote 3", 8), r.text())
n = r.text().count("soft reboot")
open(os.path.join(D, "other.txt"), "w").write("host")
time.sleep(2.5)
check("no reload while Python owns the drive", r.text().count("soft reboot") == n, r.text())
r.stop()
print("DONE")
