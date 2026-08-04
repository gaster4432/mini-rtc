import subprocess, sys, time, os

os.chdir(os.path.dirname(os.path.abspath(__file__)))

host = subprocess.Popen([sys.executable, "mic_link.py", "--headless", "host"],
                        stdout=open("host.log", "wb"), stderr=subprocess.STDOUT)
time.sleep(1)
guest = subprocess.Popen([sys.executable, "mic_link.py", "--headless", "guest"],
                         stdout=open("guest.log", "wb"), stderr=subprocess.STDOUT)

try:
    time.sleep(12)
finally:
    host.terminate(); guest.terminate()
    time.sleep(1)
    host.kill(); guest.kill()

print("=== HOST LOG ===")
print(open("host.log", encoding="utf-8", errors="replace").read())
print("=== GUEST LOG ===")
print(open("guest.log", encoding="utf-8", errors="replace").read())
