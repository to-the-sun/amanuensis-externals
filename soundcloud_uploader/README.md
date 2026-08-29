# SoundCloud Automated Uploader

A robust, automatic script that scans a local directory for audio tracks, identifies newly added ones (using MD5 hashes & filepaths saved in a local SQLite database), and uploads them via standard SoundCloud OAuth 2.1 PKCE authorization.

## How to Get SoundCloud API Credentials

Although the self-serve developer portal is currently closed for new application sign-ups on the website, you can easily obtain API credentials (`client_id` and `client_secret`) by sending a brief email to **SoundCloud Support** (`support@support.soundcloud.com`).

In your email, provide the following details:
- **SoundCloud account**: The email/profile URL of the account under which the app should be created.
- **App Name**: e.g., "My Automated Folder Uploader".
- **App Description**: e.g., "An automated script that uploads finished/work-in-progress songs from my local computer directory."
- **App Purpose**: "Personal workflow automation and uploading tracks from my local folder."
- **Redirect URI**: `http://localhost:8080/callback`

They typically respond and provision your application within **48 to 72 hours**. Once approved, your application credentials will appear on [soundcloud.com/you/apps](https://soundcloud.com/you/apps).

---

## Configuration

All configuration is done via `soundcloud_config.json`. Place your credentials and target folder inside it:

```json
{
  "client_id": "YOUR_SOUNDCLOUD_CLIENT_ID",
  "client_secret": "YOUR_SOUNDCLOUD_CLIENT_SECRET",
  "redirect_uri": "http://localhost:8080/callback",
  "access_token": "",
  "refresh_token": "",
  "expires_at": 0,
  "folder_path": "./tracks_to_upload"
}
```

- `folder_path` points to the directory containing your audio files. The uploader supports common audio formats like `.mp3`, `.wav`, `.flac`, `.aiff`, `.ogg`, `.aac`, `.m4a`, etc.

---

## Usage

### 1. Run the Authentication Flow
Authenticate your SoundCloud account using OAuth 2.1 PKCE:
```bash
python3 soundcloud_uploader.py auth
```
This command will:
1. Generate the cryptographically secure PKCE verifier and challenge.
2. Spin up a temporary local HTTP server on the port matching your `redirect_uri` (e.g., port `8080`).
3. Automatically open your default web browser to SoundCloud's authorization page.
4. Prompt you to authorize the application. Once approved, SoundCloud redirects back to the local callback server, which captures the authorization code, exchanges it for long-lived OAuth tokens, and saves them automatically to your `soundcloud_config.json`.

### 2. Scan and Upload Audio Tracks
To scan the configured folder and upload any newly discovered files:
```bash
python3 soundcloud_uploader.py upload
```
The script will:
- Initialize/connect to a local SQLite database (`soundcloud_tracks.db`) to check if the files have already been successfully uploaded in the past.
- Automatically refresh your short-lived access token if it has expired (or is close to expiring).
- Upload any non-recorded/new tracks to your SoundCloud account.
- Safely log the successfully completed uploads into the database to ensure zero duplicate uploads if run again.

---

## Requirements

Ensure you have the Python `requests` library installed:
```bash
pip install requests
```
