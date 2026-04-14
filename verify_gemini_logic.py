import subprocess
import os

def check_symbols(filepath):
    print(f"Checking symbols for {filepath}...")
    try:
        # Using nm from the mingw toolchain if available, otherwise just use nm
        nm_cmd = "x86_64-w64-mingw32-nm"
        result = subprocess.run([nm_cmd, filepath], capture_output=True, text=True)
        if result.returncode != 0:
             result = subprocess.run(["nm", filepath], capture_output=True, text=True)

        if result.returncode == 0:
            symbols = result.stdout
            if "ext_main" in symbols:
                print("SUCCESS: Found ext_main symbol.")
            else:
                print("FAILURE: Could not find ext_main symbol.")

            # Check for WinHTTP symbols
            if "WinHttpOpen" in symbols or "__imp_WinHttpOpen" in symbols:
                print("SUCCESS: Found WinHttpOpen (linked).")
            else:
                print("FAILURE: Could not find WinHttpOpen symbol.")
        else:
            print(f"Error running nm: {result.stderr}")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    dll_path = "gemini/gemini.mxe64"
    if os.path.exists(dll_path):
        check_symbols(dll_path)
    else:
        print(f"File {dll_path} not found.")
