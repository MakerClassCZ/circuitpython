import os, shutil, signal, subprocess, sys, time
# Usage: test_stellar.py circuitpython [picostellar CIRCUITPY directory]
BIN = sys.argv[1]; T = os.path.dirname(os.path.abspath(__file__)); D = os.path.join(T, "STELLAR")
SRC = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("PICOSTELLAR_DRIVE", "")
if not os.path.isdir(SRC):
    print("SKIP picostellar: give its CIRCUITPY directory as the second argument (github.com/MakerClassCZ/picostellar)")
    sys.exit(0)
shutil.rmtree(D, ignore_errors=True)
shutil.copytree(SRC, D)
shot = os.path.join(T, "stellar.bmp")
if os.path.exists(shot): os.remove(shot)
log = open(os.path.join(T, "stellar.txt"), "wb")
p = subprocess.Popen([BIN, "--drive", D], stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                     env=dict(os.environ, SDL_VIDEODRIVER="dummy", CIRCUITPY_SCREENSHOT=shot))
try:
    time.sleep(8)
    maps = [l.split()[-1] for l in open("/proc/%d/maps" % p.pid) if "sky.bin" in l] if os.path.exists("/proc/%d/maps" % p.pid) else ["(not checked on Windows)"]
    print("sky.bin mapped:", maps)
    p.send_signal(signal.SIGUSR1); time.sleep(1)
    print("screenshot:", os.path.exists(shot) and os.path.getsize(shot))
finally:
    p.kill(); p.wait()
t = open(os.path.join(T, "stellar.txt"), "rb").read().decode(errors="replace")
print("errors:", [l for l in t.splitlines() if "Error" in l or "Traceback" in l][:5])
print("tail:", t[-300:].replace("\x1b", "^["))
