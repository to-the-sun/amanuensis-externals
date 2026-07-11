import sys
import traceback

def run_bot():
    import discord
    import json
    import os
    import asyncio
    import re
    import shutil
    import threading
    from datetime import datetime, timezone, timedelta
    from pydub import AudioSegment

    # Add the parent directory of this script's directory to sys.path so we can import analyze_files
    # This assumes analyze_files.py is in the same directory as Amanuensis.py
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    try:
        import analyze_files
    except ImportError:
        sys.path.append(r'D:\[Library]\[Documents]\Max 8\Library\analyze~\python')
        import analyze_files

    # Global configuration
    DEFAULT_CHANNEL = "works-in-progress"  # Channel where the bot will post updates
    CHECK_INTERVAL = 30  # seconds
    VIDEO_OUTPUT_DIR = r'D:\[Library]\[Video]\[Works]\[Uploads]'

    # Load credentials from a separate file
    try:
        with open('credentials.json') as f:
            credentials = json.load(f)
    except FileNotFoundError:
        print("Error: credentials.json not found.")
        return

    intents = discord.Intents.default()

    class MyClient(discord.Client):
        async def setup_hook(self):
            self.loop.create_task(periodic_task())

    UPLOADS_DIR = r'D:\[Library]\[Audio]\[Works]'
    PROJECTS_DIR = r'D:\[Library]\[Audio]\[Works]\[Projects]'
    MAX_FILE_SIZE = 10 * 1024 * 1024  # 10 MB
    RENDERING_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rendering_log.txt')
    log_lock = threading.Lock()

    def get_rendering_log():
        with log_lock:
            if not os.path.exists(RENDERING_LOG):
                return []
            with open(RENDERING_LOG, 'r', encoding='utf-8') as f:
                return [line.strip() for line in f if line.strip()]

    def add_to_rendering_log(file_path):
        with log_lock:
            log = []
            if os.path.exists(RENDERING_LOG):
                with open(RENDERING_LOG, 'r', encoding='utf-8') as f:
                    log = [line.strip() for line in f if line.strip()]
            if file_path not in log:
                with open(RENDERING_LOG, 'a', encoding='utf-8') as f:
                    f.write(f"{file_path}\n")
                print(f"Added to rendering log: {file_path}")

    def remove_from_rendering_log(file_path):
        with log_lock:
            if not os.path.exists(RENDERING_LOG):
                return
            with open(RENDERING_LOG, 'r', encoding='utf-8') as f:
                log = [line.strip() for line in f if line.strip()]
            if file_path in log:
                log.remove(file_path)
                with open(RENDERING_LOG, 'w', encoding='utf-8') as f:
                    for path in log:
                        f.write(f"{path}\n")
                print(f"Removed from rendering log: {file_path}")

    def filename_segments(filename):
        return [segment.lower() for segment in re.split(r'[\W_]+', filename) if segment]

    async def delete_previous_messages(channel, file_name):
        file_segments = filename_segments(file_name)
        async for message in channel.history(limit=None):  # No limit
            if message.author == client.user:
                for attachment in message.attachments:
                    attachment_segments = filename_segments(attachment.filename)
                    if set(file_segments) == set(attachment_segments):
                        print(f'Match found: {file_segments} is {attachment_segments}')  # Log match
                        await message.delete()

    async def get_description(file_name):
        project_folder = os.path.join(PROJECTS_DIR, file_name)
        description_file = os.path.join(project_folder, 'description.txt')
        if os.path.exists(description_file):
            with open(description_file, 'r', encoding='utf-8') as f:
                lines = f.readlines()
                formatted_lines = []
                for line in lines:
                    words = line.strip().split()
                    formatted_line = '> ' + ' '.join([f'**{word}**' if word.lower() not in ['by', 'written', 'written by'] else word for word in words])
                    formatted_lines.append(formatted_line)
                return '\n'.join(formatted_lines)
        return ''

    async def get_last_post_time(channel):
        async for message in channel.history(limit=None):  # No limit
            if message.author == client.user:
                return message.created_at
        return None

    def convert_wav_to_mp3(wav_path, mp3_path):
        audio = AudioSegment.from_wav(wav_path)
        duration_seconds = len(audio) / 1000
        max_bitrate_kbps = (MAX_FILE_SIZE * 8) / duration_seconds / 1000  # Calculate max bitrate in kbps
        bitrate = min(max_bitrate_kbps - 19, 320)  # Cap the bitrate at 320 kbps
        estimated_size = (bitrate * 1000 / 8) * duration_seconds  # Estimate file size in bytes
        audio = audio.set_channels(1)
        audio.export(mp3_path, format="mp3", bitrate=f"{int(bitrate)}k")
        print(f'Converted {wav_path} to {mp3_path} with bitrate {int(bitrate)}k')
        print(f'Estimated file size: {estimated_size / (1024 * 1024):.2f} MB')

    def process_transient_analysis(file_path):
        """
        Synchronous helper to perform CPU-intensive transient analysis and video generation.
        Designed to be run in a separate thread.
        """
        try:
            print(f"Performing transient analysis for {os.path.basename(file_path)}...")
            analysis_data = analyze_files.analyze_audio(file_path)
            if analysis_data:
                video_path = analyze_files.generate_video(file_path, analysis_data)
                if video_path and os.path.exists(video_path):
                    if not os.path.exists(VIDEO_OUTPUT_DIR):
                        os.makedirs(VIDEO_OUTPUT_DIR, exist_ok=True)
                    dest_path = os.path.join(VIDEO_OUTPUT_DIR, os.path.basename(video_path))
                    shutil.move(video_path, dest_path)
                    print(f"Moved video to {dest_path}")
        except Exception as e:
            print(f"Error during transient analysis processing for {file_path}: {e}")
        finally:
            remove_from_rendering_log(file_path)

    async def periodic_task():
        await client.wait_until_ready()
        channel = discord.utils.get(client.get_all_channels(), name=DEFAULT_CHANNEL)
        if not channel:
            print(f"Channel {DEFAULT_CHANNEL} not found!")
            return

        # Resume interrupted renders
        log = get_rendering_log()
        if log:
            print(f"Found {len(log)} interrupted renders. Resuming...")
            for file_path in log:
                if os.path.exists(file_path):
                    print(f"Resuming render for {file_path}")
                    asyncio.create_task(asyncio.to_thread(process_transient_analysis, file_path))
                else:
                    print(f"File {file_path} no longer exists. Removing from rendering log.")
                    remove_from_rendering_log(file_path)

        while not client.is_closed():
            try:
                for guild in client.guilds:
                    for channel in guild.text_channels:
                        if channel.name == DEFAULT_CHANNEL:
                            last_post_time = await get_last_post_time(channel)
                            wav_files = [f for f in os.listdir(UPLOADS_DIR) if f.endswith('.wav')]
                            for wav_file in wav_files:
                                file_path = os.path.join(UPLOADS_DIR, wav_file)
                                file_name = os.path.splitext(wav_file)[0]
                                file_modified_time = datetime.fromtimestamp(os.path.getmtime(file_path), tz=timezone.utc)
                                file_created_time = datetime.fromtimestamp(os.path.getctime(file_path), tz=timezone.utc)

                                comparison_time = last_post_time if last_post_time else None

                                print(f"Checking {wav_file}:")
                                print(f"  Created:  {file_created_time}")
                                print(f"  Modified: {file_modified_time}")
                                print(f"  Compared against Last Post: {comparison_time}")

                                if comparison_time and (file_modified_time <= comparison_time and file_created_time <= comparison_time):
                                    continue
                                add_to_rendering_log(file_path)
                                print(f'Converting {wav_file} to MP3.')
                                mp3_path = os.path.join(UPLOADS_DIR, f"{file_name}.mp3")
                                convert_wav_to_mp3(file_path, mp3_path)
                                if os.path.getsize(mp3_path) > MAX_FILE_SIZE:
                                    print(f'Skipping {mp3_path} as it exceeds the file size limit.')
                                    os.remove(mp3_path)
                                    remove_from_rendering_log(file_path)
                                    continue
                                await delete_previous_messages(channel, f"{file_name}.mp3")
                                description = await get_description(file_name)
                                message_content = f"Here's the newest version of **{file_name}**"
                                if description:
                                    message_content += f'\n{description}'

                                await channel.send(
                                    content=message_content,
                                    file=discord.File(mp3_path)
                                )
                                os.remove(mp3_path)

                                # Clean up general channel and post notification
                                for ch in guild.text_channels:
                                    if ch.name == "general":
                                        # Delete all previous bot messages in general
                                        async for message in ch.history(limit=None):
                                            if message.author == client.user:
                                                await message.delete()
                                                await asyncio.sleep(0.5)  # Add small delay to avoid rate limiting

                                        await ch.send(f"{channel.mention} updated with the newest version of **{file_name}**")
                                        await asyncio.sleep(1)
                                        break

                                # Perform transient analysis and generate video in a separate thread backgrounded to avoid blocking the heartbeat
                                asyncio.create_task(asyncio.to_thread(process_transient_analysis, file_path))

                                await asyncio.sleep(2)  # Add delay to avoid rate limiting

                await asyncio.sleep(CHECK_INTERVAL)
            except Exception as e:
                print(f"Error during periodic task: {e}")

    client = MyClient(intents=intents)

    @client.event
    async def on_ready():
        print(f'We have logged in as {client.user}')

    client.run(credentials['token'])

if __name__ == "__main__":
    try:
        run_bot()
    except Exception as e:
        print("\n" + "="*60)
        print("CRITICAL ERROR")
        print("="*60)
        traceback.print_exc()
        print("="*60)
    finally:
        # Keep window open for user to see output/errors
        try:
            input("\nPress Enter to exit...")
        except (EOFError, KeyboardInterrupt):
            pass
