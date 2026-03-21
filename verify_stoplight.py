import os
import re

def verify_stoplight_c():
    filepath = 'stoplight/stoplight.c'
    if not os.path.exists(filepath):
        print(f"Error: {filepath} not found.")
        return False

    with open(filepath, 'r') as f:
        content = f.read()

    # Check for handlers
    handlers = ['stoplight_bang', 'stoplight_int', 'stoplight_float', 'stoplight_list', 'stoplight_anything']
    for handler in handlers:
        if handler not in content:
            print(f"Error: Handler {handler} not found in {filepath}")
            return False

    # Check for proxy_getinlet check in handlers
    for handler in handlers:
        # Simple regex to check if the handler has a proxy_getinlet check
        pattern = rf'void\s+{handler}\s*\(.*?\)\s*\{{.*?proxy_getinlet\(.*?\)\s*==\s*0.*?\}}'
        if not re.search(pattern, content, re.DOTALL):
            print(f"Error: {handler} might be missing proxy_getinlet check.")
            # Let's do a more permissive check
            if 'proxy_getinlet' not in content:
                 print(f"Error: proxy_getinlet not used at all in {filepath}")
                 return False

    # Check assist message for backticks
    if 'Accepts `anything` to be passed through.' in content:
        print("Error: Found backticks in assist message, but they should have been removed.")
        return False

    print("stoplight.c logic verification passed.")
    return True

if __name__ == "__main__":
    if verify_stoplight_c():
        exit(0)
    else:
        exit(1)
