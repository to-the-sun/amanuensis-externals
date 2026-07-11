import sys
import os
import unittest
import asyncio
from unittest.mock import MagicMock, patch, mock_open, AsyncMock
from datetime import datetime, timezone

# Add the script's directory to sys.path
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

# We will implement a custom Client class to ensure methods are bound correctly
class MockDiscordClient:
    def __init__(self, *args, **kwargs):
        self.user = MagicMock()
        self.loop = MagicMock()
        self.guilds = []

    def run(self, token):
        # We will patch or override this in the test
        pass

    def event(self, func):
        # decorator
        return func

# Fully mock discord and other external dependencies before importing anything
mock_discord = MagicMock()
mock_discord.Intents.default.return_value = MagicMock()
mock_discord.Client = MockDiscordClient

# Mock discord.File
class MockFile:
    def __init__(self, fp, *args, **kwargs):
        self.fp = fp

mock_discord.File = MockFile
sys.modules['discord'] = mock_discord

mock_pydub = MagicMock()
mock_audio_segment = MagicMock()
mock_audio_segment.__len__.return_value = 10000  # 10 seconds duration to avoid zero division
mock_pydub.AudioSegment.from_wav.return_value = mock_audio_segment
sys.modules['pydub'] = mock_pydub

# Also mock analyze_files since we don't want to run heavy transient analysis
mock_analyze_files = MagicMock()
sys.modules['analyze_files'] = mock_analyze_files

import Amanuensis

class TestAmanuensisIntegration(unittest.TestCase):
    def setUp(self):
        self.creds_data = '{"token": "fake_token_123"}'
        self.rendering_log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rendering_log.txt')
        # Clean up existing log file if any
        if os.path.exists(self.rendering_log_path):
            os.remove(self.rendering_log_path)

    def tearDown(self):
        if os.path.exists(self.rendering_log_path):
            os.remove(self.rendering_log_path)

    @patch('asyncio.sleep', new_callable=AsyncMock)
    @patch('builtins.open', new_callable=mock_open)
    @patch('os.path.exists')
    @patch('os.listdir')
    @patch('os.path.getmtime')
    @patch('os.path.getctime')
    @patch('os.path.getsize')
    @patch('os.remove')
    @patch('shutil.move')
    def test_periodic_task_with_skipped_file(self, mock_move, mock_os_remove, mock_getsize, mock_getctime, mock_getmtime, mock_listdir, mock_exists, mock_file_open, mock_sleep):
        """Test that a file identified for upload is logged, then removed from the log if skipped due to size limits."""
        rendering_log_content = []

        def custom_open(path, mode='r', *args, **kwargs):
            if 'credentials.json' in path:
                return mock_open(read_data=self.creds_data).return_value
            elif 'rendering_log.txt' in path:
                m = mock_open().return_value
                if 'w' in mode:
                    def write_side_effect(data):
                        rendering_log_content.clear()
                        for line in data.split('\n'):
                            line_s = line.strip()
                            if line_s:
                                rendering_log_content.append(line_s)
                    m.write.side_effect = write_side_effect
                elif 'a' in mode:
                    def write_side_effect(data):
                        for line in data.split('\n'):
                            line_s = line.strip()
                            if line_s:
                                rendering_log_content.append(line_s)
                    m.write.side_effect = write_side_effect
                else:
                    read_data = "\n".join(rendering_log_content)
                    m.read.return_value = read_data
                    m.__iter__.return_value = [line + '\n' for line in rendering_log_content]
                return m
            elif 'description.txt' in path:
                return mock_open(read_data="This is a test description by test artist").return_value
            return mock_open().return_value

        mock_file_open.side_effect = custom_open

        def custom_exists(path):
            if 'credentials.json' in path:
                return True
            elif 'rendering_log.txt' in path:
                return len(rendering_log_content) > 0
            elif 'description.txt' in path:
                return True
            return True

        mock_exists.side_effect = custom_exists
        mock_listdir.return_value = ['song1.wav']
        mock_getmtime.return_value = 1600000000.0
        mock_getctime.return_value = 1600000000.0

        # Make the file size large so it is skipped
        # MAX_FILE_SIZE is 10 * 1024 * 1024 (10 MB). Let's make it 11 MB.
        mock_getsize.return_value = 11 * 1024 * 1024

        captured_coro = None

        # Setup mock for discord client.is_closed to run loop only once
        is_closed_calls = [False, True]
        def mock_is_closed():
            if is_closed_calls:
                return is_closed_calls.pop(0)
            return True

        # Custom run implementation to capture periodic_task and execute it
        def fake_run(client_self, token):
            nonlocal captured_coro
            client_self.is_closed = mock_is_closed

            # Setup mock channels and guilds on client_self
            mock_channel_wip = MagicMock()
            mock_channel_wip.name = "works-in-progress"
            mock_channel_wip.send = AsyncMock()
            mock_channel_wip.delete = AsyncMock()

            mock_channel_general = MagicMock()
            mock_channel_general.name = "general"
            mock_channel_general.send = AsyncMock()
            mock_channel_general.delete = AsyncMock()

            # Message history mock
            comparison_datetime = datetime.fromtimestamp(1500000000.0, tz=timezone.utc)
            mock_message = MagicMock()
            mock_message.author = client_self.user
            mock_message.created_at = comparison_datetime

            class AsyncIterator:
                def __init__(self, items):
                    self.items = items
                def __aiter__(self):
                    return self
                async def __anext__(self):
                    if not self.items:
                        raise StopAsyncIteration
                    return self.items.pop(0)

            mock_channel_wip.history.return_value = AsyncIterator([mock_message])
            mock_channel_general.history.return_value = AsyncIterator([])

            mock_guild = MagicMock()
            mock_guild.text_channels = [mock_channel_wip, mock_channel_general]
            client_self.guilds = [mock_guild]
            client_self.get_all_channels = MagicMock(return_value=[mock_channel_wip])

            # Mock client wait_until_ready
            client_self.wait_until_ready = AsyncMock()

            # Mock create_task to capture periodic_task
            def mock_create_task(coro):
                nonlocal captured_coro
                captured_coro = coro
                return MagicMock()

            client_self.loop = MagicMock()
            client_self.loop.create_task.side_effect = mock_create_task

            # Trigger setup_hook
            asyncio.run(client_self.setup_hook())

        # Patch MockDiscordClient's run method
        with patch.object(MockDiscordClient, 'run', fake_run):
            Amanuensis.run_bot()

        # Execute the captured periodic_task
        self.assertIsNotNone(captured_coro)

        # Track logging actions during execution
        original_add = rendering_log_content.append
        logged_items = []
        def track_append(item):
            logged_items.append(item)
            original_add(item)

        # Patch the file list/write actions to track appends
        def tracking_write(data):
            for line in data.split('\n'):
                line_s = line.strip()
                if line_s:
                    logged_items.append(line_s)
                    rendering_log_content.append(line_s)

        # Override open to use tracking_write for 'a' mode
        def tracking_open(path, mode='r', *args, **kwargs):
            if 'credentials.json' in path:
                return mock_open(read_data=self.creds_data).return_value
            elif 'rendering_log.txt' in path:
                m = mock_open().return_value
                if 'w' in mode:
                    def write_side_effect(data):
                        rendering_log_content.clear()
                        for line in data.split('\n'):
                            line_s = line.strip()
                            if line_s:
                                rendering_log_content.append(line_s)
                    m.write.side_effect = write_side_effect
                elif 'a' in mode:
                    m.write.side_effect = tracking_write
                else:
                    read_data = "\n".join(rendering_log_content)
                    m.read.return_value = read_data
                    m.__iter__.return_value = [line + '\n' for line in rendering_log_content]
                return m
            elif 'description.txt' in path:
                return mock_open(read_data="This is a test description by test artist").return_value
            return mock_open().return_value

        mock_file_open.side_effect = tracking_open

        # Let's run periodic_task and verify that the file was added to log, and then removed
        asyncio.run(captured_coro)

        # Verify that song1.wav was added to the rendering log (since it was identified for upload)
        self.assertTrue(any("song1.wav" in item for item in logged_items), f"Expected song1.wav in logged items: {logged_items}")
        # Verify that it was removed because size exceeded limit
        self.assertNotIn("song1.wav", rendering_log_content)

if __name__ == '__main__':
    unittest.main()
