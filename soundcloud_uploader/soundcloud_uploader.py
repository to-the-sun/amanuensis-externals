import os
import sys
import json
import sqlite3
import hashlib
import time
import secrets
import base64
import urllib.parse
import webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler
import requests

# Supported audio extensions
SUPPORTED_EXTENSIONS = {'.mp3', '.wav', '.flac', '.aiff', '.aif', '.ogg', '.aac', '.m4a', '.mp2', '.amr', '.wma'}

CONFIG_FILE = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'soundcloud_config.json')
DB_FILE = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'soundcloud_tracks.db')

def load_config():
    if not os.path.exists(CONFIG_FILE):
        print(f"Configuration file not found: {CONFIG_FILE}. Creating template.")
        save_config({
            "client_id": "YOUR_SOUNDCLOUD_CLIENT_ID",
            "client_secret": "YOUR_SOUNDCLOUD_CLIENT_SECRET",
            "redirect_uri": "http://localhost:8080/callback",
            "access_token": "",
            "refresh_token": "",
            "expires_at": 0,
            "folder_path": "./tracks_to_upload"
        })
    with open(CONFIG_FILE, 'r') as f:
        return json.load(f)

def save_config(config):
    with open(CONFIG_FILE, 'w') as f:
        json.dump(config, f, indent=2)

def init_db():
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS uploads (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            filepath TEXT,
            filename TEXT,
            file_hash TEXT UNIQUE,
            soundcloud_track_id TEXT,
            uploaded_at REAL
        )
    ''')
    conn.commit()
    conn.close()

def calculate_file_hash(filepath):
    """Calculates MD5 hash of a file to uniquely identify it even if renamed."""
    hasher = hashlib.md5()
    with open(filepath, 'rb') as f:
        # Read in blocks of 64kb
        for chunk in iter(lambda: f.read(65536), b''):
            hasher.update(chunk)
    return hasher.hexdigest()

def is_already_uploaded(filepath, file_hash):
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute('SELECT soundcloud_track_id FROM uploads WHERE filepath = ? OR file_hash = ?', (filepath, file_hash))
    row = cursor.fetchone()
    conn.close()
    return row[0] if row else None

def record_upload(filepath, filename, file_hash, track_id):
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute('''
        INSERT OR REPLACE INTO uploads (filepath, filename, file_hash, soundcloud_track_id, uploaded_at)
        VALUES (?, ?, ?, ?, ?)
    ''', (filepath, filename, file_hash, str(track_id), time.time()))
    conn.commit()
    conn.close()

def scan_folder(folder_path):
    if not os.path.exists(folder_path):
        print(f"Warning: Folder path '{folder_path}' does not exist.")
        return []
    audio_files = []
    for root, _, files in os.walk(folder_path):
        for file in files:
            _, ext = os.path.splitext(file)
            if ext.lower() in SUPPORTED_EXTENSIONS:
                audio_files.append(os.path.join(root, file))
    return audio_files

def generate_pkce_verifier():
    chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"
    return "".join(secrets.choice(chars) for _ in range(64))

def generate_pkce_challenge(verifier):
    sha256_hash = hashlib.sha256(verifier.encode('utf-8')).digest()
    challenge = base64.urlsafe_b64encode(sha256_hash).decode('utf-8')
    return challenge.rstrip('=')

class OAuthCallbackHandler(BaseHTTPRequestHandler):
    """Temporary server handler to capture authorization code from browser callback."""
    code = None
    error = None

    def do_GET(self):
        parsed_url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed_url.query)

        if 'code' in query:
            OAuthCallbackHandler.code = query['code'][0]
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            self.wfile.write(b"<h1>Authentication Successful!</h1><p>You can close this tab now and return to the terminal.</p>")
        elif 'error' in query:
            OAuthCallbackHandler.error = query['error'][0]
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            self.wfile.write(f"<h1>Authentication Failed!</h1><p>Error: {OAuthCallbackHandler.error}</p>".encode('utf-8'))
        else:
            self.send_response(400)
            self.end_headers()

    def log_message(self, format, *args):
        # Suppress logging server requests to terminal to keep it clean
        pass

def run_local_server(port=8080):
    server_address = ('', port)
    httpd = HTTPServer(server_address, OAuthCallbackHandler)
    print(f"Starting callback listener on port {port}...")
    while OAuthCallbackHandler.code is None and OAuthCallbackHandler.error is None:
        httpd.handle_request()
    httpd.server_close()
    return OAuthCallbackHandler.code, OAuthCallbackHandler.error

def run_auth_flow():
    config = load_config()
    client_id = config.get("client_id")
    client_secret = config.get("client_secret")
    redirect_uri = config.get("redirect_uri")

    if not client_id or client_id.startswith("YOUR_"):
        print("Error: Please set your valid 'client_id' and 'client_secret' in soundcloud_config.json first.")
        print("See README.md on how to request/get these credentials.")
        return False

    verifier = generate_pkce_verifier()
    challenge = generate_pkce_challenge(verifier)

    parsed_redirect = urllib.parse.urlparse(redirect_uri)
    port = parsed_redirect.port if parsed_redirect.port else 8080

    params = {
        "client_id": client_id,
        "redirect_uri": redirect_uri,
        "response_type": "code",
        "code_challenge": challenge,
        "code_challenge_method": "S256",
        "scope": "non-expiring"
    }

    auth_url = f"https://secure.soundcloud.com/authorize?{urllib.parse.urlencode(params)}"
    print("\n" + "="*80)
    print("SoundCloud OAuth Authentication")
    print("="*80)
    print("Opening browser to authorize application...")
    print(f"If the browser doesn't open automatically, please open this URL:\n{auth_url}\n")

    webbrowser.open(auth_url)

    # Start local server to capture redirect
    code, error = run_local_server(port)

    if error:
        print(f"Authorization failed: {error}")
        return False

    if not code:
        print("No authorization code retrieved.")
        return False

    print("Exchanging authorization code for tokens...")
    token_url = "https://secure.soundcloud.com/oauth/token"
    token_data = {
        "grant_type": "authorization_code",
        "client_id": client_id,
        "client_secret": client_secret,
        "redirect_uri": redirect_uri,
        "code": code,
        "code_verifier": verifier
    }

    resp = requests.post(token_url, data=token_data)
    if resp.status_code != 200:
        print(f"Token exchange failed: {resp.status_code} {resp.text}")
        return False

    tokens = resp.json()
    config["access_token"] = tokens.get("access_token")
    config["refresh_token"] = tokens.get("refresh_token")
    expires_in = tokens.get("expires_in", 3600)
    config["expires_at"] = time.time() + expires_in
    save_config(config)

    print("Success! Tokens obtained and saved in soundcloud_config.json.")
    return True

def refresh_access_token():
    config = load_config()
    client_id = config.get("client_id")
    client_secret = config.get("client_secret")
    refresh_token = config.get("refresh_token")

    if not refresh_token:
        print("No refresh token found. Please run authentication flow.")
        return False

    print("Refreshing access token...")
    token_url = "https://secure.soundcloud.com/oauth/token"
    token_data = {
        "grant_type": "refresh_token",
        "client_id": client_id,
        "client_secret": client_secret,
        "refresh_token": refresh_token
    }

    resp = requests.post(token_url, data=token_data)
    if resp.status_code != 200:
        print(f"Token refresh failed: {resp.status_code} {resp.text}")
        return False

    tokens = resp.json()
    config["access_token"] = tokens.get("access_token")
    if tokens.get("refresh_token"):
        config["refresh_token"] = tokens.get("refresh_token")
    expires_in = tokens.get("expires_in", 3600)
    config["expires_at"] = time.time() + expires_in
    save_config(config)
    print("Access token successfully refreshed!")
    return True

def ensure_valid_token():
    config = load_config()
    expires_at = config.get("expires_at", 0)
    # If token expires in less than 5 minutes, refresh it
    if time.time() > (expires_at - 300):
        if config.get("refresh_token"):
            return refresh_access_token()
        else:
            print("Access token expired and no refresh token is available.")
            return False
    return True

def upload_track(filepath):
    if not ensure_valid_token():
        print("Cannot upload track: authentication token is invalid or expired.")
        return False

    config = load_config()
    access_token = config.get("access_token")

    filename = os.path.basename(filepath)
    title, _ = os.path.splitext(filename)

    print(f"\nUploading '{filename}' to SoundCloud...")

    upload_url = "https://api.soundcloud.com/tracks"
    headers = {
        "Authorization": f"OAuth {access_token}",
        "Accept": "application/json; charset=utf-8"
    }

    # Multipart form-data payload
    # Note: track[asset_data] is the binary audio file
    files = {
        "track[asset_data]": (filename, open(filepath, "rb"), "audio/mpeg" if filepath.endswith(".mp3") else "application/octet-stream")
    }

    data = {
        "track[title]": title,
        "track[sharing]": "public" # default sharing status
    }

    try:
        resp = requests.post(upload_url, headers=headers, data=data, files=files)
        # Close the open file handle
        files["track[asset_data]"][1].close()

        if resp.status_code in (200, 201):
            track_info = resp.json()
            track_id = track_info.get("id")
            print(f"Upload complete! Track '{title}' uploaded with SoundCloud Track ID: {track_id}")
            return track_id
        else:
            print(f"Upload failed (HTTP {resp.status_code}): {resp.text}")
            return None
    except Exception as e:
        print(f"An unexpected error occurred during upload: {e}")
        return None

def run_main_uploader():
    init_db()
    config = load_config()
    folder_path = config.get("folder_path")

    if not folder_path or folder_path.startswith("YOUR_") or not os.path.exists(folder_path):
        # Create folder if it doesn't exist
        os.makedirs(folder_path, exist_ok=True)
        print(f"Created default folder: '{folder_path}'. Please place audio tracks inside it.")

    print(f"Scanning folder '{folder_path}'...")
    audio_files = scan_folder(folder_path)

    if not audio_files:
        print("No audio files found to upload.")
        return

    print(f"Found {len(audio_files)} audio file(s). Processing uploads...")

    for filepath in audio_files:
        filename = os.path.basename(filepath)
        file_hash = calculate_file_hash(filepath)

        existing_id = is_already_uploaded(filepath, file_hash)
        if existing_id:
            print(f"Skipping '{filename}': Already uploaded with Track ID {existing_id}.")
            continue

        track_id = upload_track(filepath)
        if track_id:
            record_upload(filepath, filename, file_hash, track_id)
            print(f"Recorded '{filename}' in database.")
        else:
            print(f"Failed to upload '{filename}'. Will retry in next cycle.")

def print_help():
    help_text = """
SoundCloud Automated Uploader

Commands:
  auth    : Runs the OAuth 2.1 PKCE flow to authenticate the app and save credentials.
  upload  : Scans the configured folder and uploads any new, unrecorded audio files.
  help    : Shows this help message.

Configuration:
  Edit 'soundcloud_config.json' to set your client_id, client_secret, redirect_uri, and folder_path.
"""
    print(help_text)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print_help()
        sys.exit(0)

    cmd = sys.argv[1].lower()
    if cmd == "auth":
        run_auth_flow()
    elif cmd == "upload":
        run_main_uploader()
    elif cmd == "help" or cmd == "--help" or cmd == "-h":
        print_help()
    else:
        print(f"Unknown command: {cmd}")
        print_help()
