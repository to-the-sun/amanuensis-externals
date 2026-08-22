import os
import json
import tempfile
import unittest
from unittest.mock import patch
import backup

class TestBackupLocalCleanup(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.projects_folder = os.path.join(self.temp_dir.name, "Projects")
        os.makedirs(self.projects_folder, exist_ok=True)

        self.project_a = os.path.join(self.projects_folder, "ProjectA")
        self.backup_dir = os.path.join(self.project_a, "Backup")
        os.makedirs(self.backup_dir, exist_ok=True)

    def tearDown(self):
        self.temp_dir.cleanup()

    @patch("backup.send2trash.send2trash")
    def test_local_cleanup_palette_in_transcript(self, mock_send2trash):
        palette_file = "palette_2023-01-01-12-00-00.wav"
        parent_palette_path = os.path.join(self.project_a, palette_file)
        with open(parent_palette_path, "w") as f:
            f.write("dummy audio")

        backup_file = os.path.join(self.backup_dir, palette_file)
        with open(backup_file, "w") as f:
            f.write("dummy backup")

        transcript_path = os.path.join(self.project_a, "transcript.json")
        transcript_content = {
            "track_1": {
                "bar_1": {
                    "palette": "2023-01-01-12-00-00.wav"
                }
            }
        }
        with open(transcript_path, "w") as f:
            json.dump(transcript_content, f)

        with patch("backup.PROJECTS_FOLDER", self.projects_folder):
            backup.local_cleanup()

        mock_send2trash.assert_not_called()

    @patch("backup.send2trash.send2trash")
    def test_local_cleanup_palette_not_in_transcript(self, mock_send2trash):
        palette_file = "palette_2023-01-01-12-00-00.wav"
        parent_palette_path = os.path.join(self.project_a, palette_file)
        with open(parent_palette_path, "w") as f:
            f.write("dummy audio")

        backup_file = os.path.join(self.backup_dir, palette_file)
        with open(backup_file, "w") as f:
            f.write("dummy backup")

        transcript_path = os.path.join(self.project_a, "transcript.json")
        transcript_content = {
            "track_1": {
                "bar_1": {
                    "palette": "other_file.wav"
                }
            }
        }
        with open(transcript_path, "w") as f:
            json.dump(transcript_content, f)

        with patch("backup.PROJECTS_FOLDER", self.projects_folder):
            backup.local_cleanup()

        mock_send2trash.assert_called_once_with(os.path.normpath(parent_palette_path))

    @patch("backup.send2trash.send2trash")
    def test_local_cleanup_missing_transcript(self, mock_send2trash):
        palette_file = "palette_2023-01-01-12-00-00.wav"
        parent_palette_path = os.path.join(self.project_a, palette_file)
        with open(parent_palette_path, "w") as f:
            f.write("dummy audio")

        backup_file = os.path.join(self.backup_dir, palette_file)
        with open(backup_file, "w") as f:
            f.write("dummy backup")

        with patch("backup.PROJECTS_FOLDER", self.projects_folder):
            backup.local_cleanup()

        mock_send2trash.assert_called_once_with(os.path.normpath(parent_palette_path))

    @patch("backup.send2trash.send2trash")
    def test_local_cleanup_non_palette_file(self, mock_send2trash):
        log_file = "log [2023-01-01-12-00-00].txt"
        parent_log_path = os.path.join(self.project_a, log_file)
        with open(parent_log_path, "w") as f:
            f.write("dummy log")

        backup_file = os.path.join(self.backup_dir, log_file)
        with open(backup_file, "w") as f:
            f.write("dummy backup")

        transcript_path = os.path.join(self.project_a, "transcript.json")
        transcript_content = {
            "palette": "log [2023-01-01-12-00-00].txt"
        }
        with open(transcript_path, "w") as f:
            json.dump(transcript_content, f)

        with patch("backup.PROJECTS_FOLDER", self.projects_folder):
            backup.local_cleanup()

        mock_send2trash.assert_called_once_with(os.path.normpath(parent_log_path))

if __name__ == "__main__":
    unittest.main()
