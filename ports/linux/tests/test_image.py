import os, shutil, subprocess, sys, time
BIN = sys.argv[1]; T = os.path.dirname(os.path.abspath(__file__))
env = dict(os.environ, SDL_VIDEODRIVER="dummy")
def run(args, wait_for, timeout=10):
    log = os.path.join(T, "img.txt")
    with open(log, "wb") as f:
        p = subprocess.Popen([BIN] + args, cwd=T, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT, env=env)
        end = time.time() + timeout; ok = False
        while time.time() < end:
            if wait_for in open(log, "rb").read().decode(errors="replace"):
                ok = True; break
            time.sleep(0.1)
        maps = []
        if os.path.exists("/proc/%d/maps" % p.pid):     # Linux: the image is memory-mapped
            maps = [l.split()[-1] for l in open("/proc/%d/maps" % p.pid) if ".img" in l or "sky.bin" in l]
        p.terminate(); p.wait()
    return ok, maps, open(log, "rb").read().decode(errors="replace")
for f in ("new.img", "demo.img"):
    if os.path.exists(os.path.join(T, f)): os.remove(os.path.join(T, f))
ok, maps, t = run(["--drive", "new.img"], "Hello World!")
print("PASS" if ok and os.path.isfile(os.path.join(T, "new.img")) else "FAIL", "new .img created, formatted, code.py runs", maps)
DEMO = os.environ.get("DEMO_IMAGE", "")
if os.path.isfile(DEMO):
    shutil.copy(DEMO, os.path.join(T, "demo.img"))
    ok, maps, t = run(["--drive", "demo.img"], "code.py output:", 10)
    print("PASS" if ok else "FAIL", "existing demo image boots", maps, [l for l in t.splitlines() if "Error" in l][:3])
os.makedirs(os.path.join(T, "plainfile"), exist_ok=True); shutil.rmtree(os.path.join(T, "plainfile"))
ok, maps, t = run(["--drive", "new.img"], "Hello World!")
print("PASS" if ok else "FAIL", "the .img is reused on a second start")
