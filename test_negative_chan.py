import socket
import json
import time
import os
import subprocess

def test():
    # Start the visualizer in headless mode
    env = os.environ.copy()
    env["HEADLESS"] = "1"
    viz_proc = subprocess.Popen(["python3", "visualize_script.py"], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    time.sleep(2) # Wait for server to start

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", 9999))

        # Test 1: Positive channel (standard behavior)
        pkt1 = {"track": 1, "ms": 100.0, "chan": 0, "val": 123.4, "num_chans": 2}
        sock.sendall((json.dumps(pkt1) + "\n").encode())

        # Test 2: Negative channel (new behavior - all channels)
        pkt2 = {"track": 2, "ms": 200.0, "chan": -2, "val": 567.8, "num_chans": 2}
        sock.sendall((json.dumps(pkt2) + "\n").encode())

        time.sleep(2)
        sock.close()

    finally:
        viz_proc.terminate()
        stdout, stderr = viz_proc.communicate()
        print("STDOUT:", stdout)
        print("STDERR:", stderr)

if __name__ == "__main__":
    test()
