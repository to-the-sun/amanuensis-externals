import sys
import os
import json
import socket
import importlib.util
import traceback

def main():
    if len(sys.argv) < 3:
        print("Usage: python_bridge.py <script_path> <port>")
        sys.exit(1)

    script_path = sys.argv[1]
    port = int(sys.argv[2])

    if not os.path.isabs(script_path):
        script_path = os.path.abspath(script_path)

    script_dir = os.path.dirname(script_path)
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)

    # Load the module
    try:
        module_name = os.path.splitext(os.path.basename(script_path))[0]
        spec = importlib.util.spec_from_file_location(module_name, script_path)
        if spec is None:
            print(f"Could not load spec for {script_path}")
            sys.exit(1)
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
    except Exception as e:
        print(f"Error loading script {script_path}: {e}")
        traceback.print_exc()
        sys.exit(1)

    # Setup socket
    try:
        server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_sock.bind(('127.0.0.1', port))
        server_sock.listen(1)
    except Exception as e:
        print(f"Error binding to port {port}: {e}")
        sys.exit(1)

    print(f"Python bridge listening on port {port}, script: {script_path}")
    sys.stdout.flush()

    try:
        conn, addr = server_sock.accept()
    except KeyboardInterrupt:
        sys.exit(0)

    with conn:
        while True:
            try:
                # Read 4-byte length prefix
                data = conn.recv(4)
                if not data:
                    break
                length = int.from_bytes(data, byteorder='big', signed=True)

                # Read payload
                payload = b""
                while len(payload) < length:
                    chunk = conn.recv(length - len(payload))
                    if not chunk:
                        break
                    payload += chunk

                if len(payload) < length:
                    break

                cmd = json.loads(payload.decode('utf-8'))
                func_name = cmd.get('func')
                args = cmd.get('args', [])

                if hasattr(module, func_name):
                    func = getattr(module, func_name)
                    try:
                        if not callable(func):
                             response = {"status": "error", "message": f"'{func_name}' is not callable"}
                        else:
                            result = func(*args)
                            # Handle multiple return values
                            # If it's a list or tuple, we treat it as multiple return values for the outlet
                            if isinstance(result, (list, tuple)):
                                res_list = list(result)
                            else:
                                res_list = [result]

                            response = {"status": "ok", "result": res_list}
                    except Exception as e:
                        response = {"status": "error", "message": str(e), "traceback": traceback.format_exc()}
                else:
                    response = {"status": "error", "message": f"Function '{func_name}' not found"}

                # Send back response
                resp_payload = json.dumps(response).encode('utf-8')
                conn.sendall(len(resp_payload).to_bytes(4, byteorder='big', signed=True))
                conn.sendall(resp_payload)

            except Exception as e:
                print(f"Error in bridge loop: {e}")
                traceback.print_exc()
                break

if __name__ == "__main__":
    main()
