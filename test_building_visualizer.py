import socket
import json
import time

TCP_PORT = 9999

def send_building_packet():
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", TCP_PORT))

        packet = {
            "building": {
                "1-1000.0": {
                    "absolutes": [1100.0, 1200.0],
                    "offsets": [1000.0],
                    "span": [0.0, 125.0, 250.0]
                },
                "2-1500.0": {
                    "absolutes": [1600.0],
                    "offsets": [1500.0],
                    "span": [0.0, 125.0]
                }
            },
            "current_offset": 1000.0,
            "bar_length": 125.0
        }

        msg = json.dumps(packet) + "\n"
        sock.sendall(msg.encode('utf-8'))
        print(f"Sent building packet: {packet}")

        sock.close()
    except Exception as e:
        print(f"Error sending packet: {e}")

if __name__ == "__main__":
    # Give the visualizer a moment to start if run concurrently
    time.sleep(1)
    send_building_packet()
