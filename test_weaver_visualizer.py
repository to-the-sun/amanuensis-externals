import socket
import json
import time

def test_visualizer():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect(("127.0.0.1", 9999))
    except Exception as e:
        print(f"Error connecting to visualizer: {e}")
        return

    print("Connected to visualizer. Sending test data...")

    # Clear state
    sock.sendall(b'{"clear": 1}\n')
    time.sleep(0.1)

    # Send some ramp data for track 1 and 2
    for i in range(100):
        ms = i * 10.0 # 10ms steps
        # Track 1: f1 goes 0->1, f2 goes 1->0
        f1 = i / 100.0
        f2 = 1.0 - f1
        pkt1 = {"track": 1, "ms": ms, "f1": f1, "f2": f2}

        # Track 2: f1 goes 1->0, f2 goes 0->1
        pkt2 = {"track": 2, "ms": ms, "f1": f2, "f2": f1}

        sock.sendall((json.dumps(pkt1) + "\n").encode())
        sock.sendall((json.dumps(pkt2) + "\n").encode())
        time.sleep(0.01)

    print("Sending track-specific clear for track 1...")
    sock.sendall(b'{"clear": 1, "track": 1}\n')
    time.sleep(0.5)

    print("Sending global clear...")
    sock.sendall(b'{"clear": 1}\n')
    time.sleep(0.5)

    print("Test data sent.")
    sock.close()

if __name__ == "__main__":
    test_visualizer()
