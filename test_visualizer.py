import socket
import json
import time

UDP_IP = "127.0.0.1"
UDP_PORT = 9999

def send_pkt(data):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(data.encode('utf-8'), (UDP_IP, UDP_PORT))

print("Sending mock data to visualizer...")

# 1. Normal JSON
send_pkt(json.dumps({"track": 1, "channel": 0, "ms": 100.0, "val": 1.0}))
time.sleep(0.1)

# 2. Multiple JSON objects in one packet
send_pkt(json.dumps({"track": 1, "channel": 1, "ms": 200.0, "val": -1.0}) + json.dumps({"track": 2, "channel": 0, "ms": 150.0, "val": 0.5}))
time.sleep(0.1)

# 3. Malformed JSON followed by good JSON
send_pkt('{"bad": 1, invalid} {"track": 3, "channel": 0, "ms": 300.0, "val": 1.0}')
time.sleep(0.1)

# 4. JSON with trailing comma (common bug in some C code)
send_pkt('{"track": 4, "channel": 0, "ms": 400.0, "val": 2.0},')
time.sleep(0.1)

# 5. Non-JSON garbage
send_pkt('Some random string that should be ignored')
time.sleep(0.1)

# 6. Removal of a point (val: 0.0)
send_pkt(json.dumps({"track": 1, "channel": 0, "ms": 100.0, "val": 0.0}))

print("Done.")
