import subprocess
import shutil
import sys
import os
import time

def run_diagnostic():
    print("=== GPU Encoding Diagnostic Tool ===")

    # 1. Try to find FFmpeg
    try:
        import static_ffmpeg
        static_ffmpeg.add_paths()
        print("static-ffmpeg detected and paths added.")
    except ImportError:
        print("static-ffmpeg not installed, using system PATH.")

    ffmpeg_bin = shutil.which("ffmpeg")
    if not ffmpeg_bin:
        print("ERROR: ffmpeg not found in PATH.")
        return

    print(f"FFmpeg binary found at: {ffmpeg_bin}")

    # 2. Check for H.264 encoders
    print("\nChecking for available H.264 encoders...")
    try:
        result = subprocess.run([ffmpeg_bin, "-encoders"], capture_output=True, text=True, check=True)
        encoders = [line for line in result.stdout.split('\n') if 'h264' in line]
        for e in encoders:
            print(f"  {e}")
    except Exception as e:
        print(f"Error listing encoders: {e}")
        return

    # 3. Attempt a test encode
    test_duration = 30  # seconds
    output_file = "gpu_test_output.mp4"

    # We'll try NVENC if present, otherwise AMF, otherwise libx264
    codec = "libx264"
    if "h264_nvenc" in result.stdout:
        codec = "h264_nvenc"
    elif "h264_amf" in result.stdout:
        codec = "h264_amf"

    print(f"\nAttempting a {test_duration}s test encode (1080p @ 30fps) using: {codec}")
    print("While this is running, check Windows Task Manager!")
    print("Go to the 'Performance' tab, select your GPU, and look for 'Video Encode' or 'Encoder'.")
    print("If you don't see it, click the arrow next to '3D' or 'Copy' and select it from the dropdown.")
    print("-" * 50)

    extra_args = []
    if codec == "h264_nvenc":
        extra_args = ['-preset', 'p1', '-tune', 'ull', '-zerolatency', '1']
    elif codec == "h264_amf":
        extra_args = ['-quality', 'speed', '-usage', 'ultralowlatency']
    else:
        extra_args = ['-preset', 'ultrafast']

    # Generate 1080p test pattern
    cmd = [
        ffmpeg_bin, "-y",
        "-f", "lavfi", "-i", f"testsrc=size=1920x1080:rate=30:duration={test_duration}",
        "-c:v", codec,
        *extra_args,
        "-pix_fmt", "yuv420p",
        output_file
    ]

    print(f"Command: {' '.join(cmd)}")

    start_time = time.time()
    try:
        # We run this and pipe stderr to see what ffmpeg is doing
        process = subprocess.Popen(cmd, stderr=subprocess.PIPE, text=True)

        while True:
            line = process.stderr.readline()
            if not line and process.poll() is not None:
                break
            if line:
                print(f"FFmpeg: {line.strip()}")

        process.wait()
        end_time = time.time()

        if process.returncode == 0:
            print("-" * 50)
            print(f"\nSUCCESS: Test encode completed in {end_time - start_time:.2f} seconds.")
            print(f"Output saved to: {os.path.abspath(output_file)}")
        else:
            print("-" * 50)
            print(f"\nFAILURE: FFmpeg exited with code {process.returncode}.")

    except Exception as e:
        print(f"\nAn error occurred: {e}")
    finally:
        if os.path.exists(output_file):
            try:
                os.remove(output_file)
            except:
                pass

if __name__ == "__main__":
    run_diagnostic()
