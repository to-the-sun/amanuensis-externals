import socket
import json
import time

UDP_PORT = 9999
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send_pkt(data):
    msg = json.dumps(data)
    sock.sendto(msg.encode('utf-8'), ("127.0.0.1", UDP_PORT))
    print(f"Sent: {msg}")

# 1. Normal data point
send_pkt({"track": 1, "channel": 0, "ms": 1000.0, "val": 250.0})
time.sleep(0.1)

# 2. Reach point (special value -999999)
send_pkt({"track": 1, "channel": 1, "ms": 2000.0, "val": -999999.0})
time.sleep(0.1)

# 3. Concatenated points (simulating high volume)
msg = json.dumps({"track": 2, "channel": 0, "ms": 500.0, "val": 100.0}) + json.dumps({"track": 2, "channel": 1, "ms": 500.0, "val": -999999.0})
sock.sendto(msg.encode('utf-8'), ("127.0.0.1", UDP_PORT))
print(f"Sent concatenated: {msg}")
time.sleep(0.1)

# 4. Update existing point
send_pkt({"track": 1, "channel": 0, "ms": 1000.0, "val": 500.0})
time.sleep(0.1)

# 5. Clear point (val = 0)
send_pkt({"track": 2, "channel": 0, "ms": 500.0, "val": 0.0})
time.sleep(0.1)

print("Test sequence finished.")
