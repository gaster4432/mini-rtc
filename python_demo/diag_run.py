import subprocess, sys, time, os, socket, json

os.chdir(os.path.dirname(os.path.abspath(__file__)))

def run(role, name, extra_env=None):
    env = dict(os.environ)
    env["ROLE"] = role
    env["DMP"] = name + ".json"
    return subprocess.Popen([sys.executable, "mic_link.py", "--headless", role],
                            stdin=subprocess.DEVNULL,
                            stdout=open(name + ".log", "wb"), stderr=subprocess.STDOUT)

host = run("host", "dhost")
time.sleep(1)
guest = run("guest", "dguest")
time.sleep(10)
host.terminate(); guest.terminate()
time.sleep(1)
host.kill(); guest.kill()

for name in ("dhost", "dguest"):
    print(f"=== {name} ===")
    print(open(name + ".log", encoding="utf-8", errors="replace").read())