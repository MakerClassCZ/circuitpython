import os, shutil, subprocess, sys, time
BIN = sys.argv[1]; T = os.path.dirname(os.path.abspath(__file__)); D = os.path.join(T, "ERASE")
shutil.rmtree(D, ignore_errors=True); os.makedirs(D)
open(os.path.join(D, "keep.txt"), "w").write("still here")
open(os.path.join(D, "code.py"), "w").write('import os, storage\nif "erased" not in os.listdir("/lib") if "lib" in os.listdir("/") else True:\n    pass\nimport microcontroller\nif microcontroller.cpu.reset_reason != microcontroller.ResetReason.SOFTWARE:\n    storage.erase_filesystem()\nprint("after erase", sorted(os.listdir("/")))\n')
log = os.path.join(T, "erase.txt")
with open(log, "wb") as f:
    p = subprocess.Popen([BIN, "--drive", D], cwd=T, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT, env=dict(os.environ, SDL_VIDEODRIVER="dummy"))
    end = time.time() + 12
    while time.time() < end and "after erase" not in open(log, "rb").read().decode(errors="replace"):
        time.sleep(0.2)
    p.terminate(); p.wait()
t = open(log, "rb").read().decode(errors="replace")
ok = "after erase" in t and open(os.path.join(D, "keep.txt")).read() == "still here"
print("PASS" if ok else "FAIL", "erase_filesystem leaves the directory", [l for l in t.splitlines() if "after erase" in l or "Error" in l][:3])
