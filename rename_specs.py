import os

def rename_in_specs():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    target_string = "assembly::transcribe::bar"
    replacement_string = "mode_duration::transcription::bar"

    print(f"Scanning for 'specs.json' files in subdirectories of: {script_dir}")
    print(f"Replacing '{target_string}' with '{replacement_string}'\n")

    found_any = False
    # os.listdir returns all files and folders in the script_dir
    for item in os.listdir(script_dir):
        item_path = os.path.join(script_dir, item)

        # Only process immediate subdirectories
        if os.path.isdir(item_path):
            specs_path = os.path.join(item_path, "specs.json")

            if os.path.isfile(specs_path):
                found_any = True
                print(f"Processing: {specs_path}")
                try:
                    with open(specs_path, 'r', encoding='utf-8') as f:
                        content = f.read()

                    if target_string in content:
                        new_content = content.replace(target_string, replacement_string)
                        with open(specs_path, 'w', encoding='utf-8') as f:
                            f.write(new_content)
                        print(f"  --> Updated '{specs_path}'")
                    else:
                        print(f"  --> String not found in '{specs_path}'")
                except Exception as e:
                    print(f"  --> Error processing '{specs_path}': {e}")

    if not found_any:
        print("No 'specs.json' files found in immediate subdirectories.")

    print("\nTask complete.")
    input("Press Enter to close...")

if __name__ == "__main__":
    rename_in_specs()
