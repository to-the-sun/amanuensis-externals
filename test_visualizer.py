import socket
import json
import time

UDP_PORT = 9999
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send_pkt(data):
    msg = json.dumps(data)
    sock.sendto(msg.encode('utf-8'), ("127.0.0.1", UDP_PORT))
    print(f"Sent: {msg}")

# 1. Normal data point at ms=1000, 2 channels
send_pkt({"track": 1, "ms": 1000.0, "chan": 0, "val": 250.0, "num_chans": 2})
time.sleep(0.1)

# 2. Reach point (special value -999999) at ms=2000
send_pkt({"track": 1, "ms": 2000.0, "chan": -1, "val": -999999.0, "num_chans": -1})
time.sleep(0.1)

# 3. Concatenated points
msg = json.dumps({"track": 2, "ms": 500.0, "chan": 1, "val": 100.0, "num_chans": 4}) + json.dumps({"track": 2, "ms": 500.0, "chan": 0, "val": -999999.0, "num_chans": 4})
sock.sendto(msg.encode('utf-8'), ("127.0.0.1", UDP_PORT))
print(f"Sent concatenated: {msg}")
time.sleep(0.1)

# 4. Point at very different time to test domain scaling (ms=5000)
send_pkt({"track": 3, "ms": 5000.0, "chan": 0, "val": 150.0, "num_chans": 1})
time.sleep(0.1)

# 5. Clear point (val = 0) at ms=500 for track 2
send_pkt({"track": 2, "ms": 500.0, "chan": 1, "val": 0.0, "num_chans": 4})
time.sleep(0.1)

print("Test sequence finished.")
