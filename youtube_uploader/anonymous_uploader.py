#!/usr/bin/env python3
import argparse
import os
import sys
import json
import time

try:
    from google_auth_oauthlib.flow import InstalledAppFlow
    from google.auth.transport.requests import Request
    from google.oauth2.credentials import Credentials
    from googleapiclient.discovery import build
    from googleapiclient.http import MediaFileUpload
    import googleapiclient.errors
except ImportError as e:
    print(f"Error: Missing required library. Please install Google API client libraries: {e}")
    sys.exit(1)

SCOPES = ["https://www.googleapis.com/auth/youtube.upload"]

def main():
    parser = argparse.ArgumentParser(
        description="Upload one or more video files to YouTube under the Music category (ID 10) with no playlist and no description."
    )
    parser.add_argument(
        "video_paths",
        nargs="+",
        help="One or more absolute or relative file paths to the video files to be uploaded."
    )
    args = parser.parse_args()

    # Pre-validate files before uploading
    validated_files = []
    for raw_path in args.video_paths:
        abs_path = os.path.abspath(raw_path)

        # Check if file exists
        if not os.path.exists(abs_path):
            print(f"Error: The file does not exist: '{abs_path}'")
            sys.exit(1)

        # Check if it is a file (not a directory)
        if not os.path.isfile(abs_path):
            print(f"Error: '{abs_path}' is not a file.")
            sys.exit(1)

        # Check if it is an MP4 file (warn if not, but accept it)
        if not abs_path.lower().endswith(".mp4"):
            print(f"Warning: '{abs_path}' does not end in .mp4. Proceeding anyway...")

        validated_files.append(abs_path)

    # Get script directory to locate anonymous_credentials.json and anonymous_token.json
    script_dir = os.path.dirname(os.path.realpath(__file__))
    credentials_path = os.path.join(script_dir, "anonymous_credentials.json")
    token_path = os.path.join(script_dir, "anonymous_token.json")

    if not os.path.exists(credentials_path):
        print(f"Error: Credentials template file not found at: '{credentials_path}'")
        print("Please ensure 'anonymous_credentials.json' exists in the same directory as this script.")
        sys.exit(1)

    # Setup Google OAuth credentials
    creds = None
    if os.path.exists(token_path):
        try:
            with open(token_path, "r") as token_file:
                token_data = json.load(token_file)
                creds = Credentials.from_authorized_user_info(token_data, SCOPES)
        except Exception as e:
            print(f"Warning: Failed to load existing token from '{token_path}': {e}")

    # If there are no valid credentials available, let the user log in.
    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            print("Refreshing access token...")
            try:
                creds.refresh(Request())
            except Exception as e:
                print(f"Error refreshing access token: {e}. Initiating login flow...")
                flow = InstalledAppFlow.from_client_secrets_file(credentials_path, SCOPES)
                creds = flow.run_local_server(port=0)
        else:
            print("No valid cached token found. Starting local authentication flow...")
            flow = InstalledAppFlow.from_client_secrets_file(credentials_path, SCOPES)
            creds = flow.run_local_server(port=0)

        # Save the credentials for the next run
        try:
            with open(token_path, "w") as token_file:
                token_file.write(creds.to_json())
            print(f"Saved authentication token to '{token_path}'")
        except Exception as e:
            print(f"Warning: Failed to save authentication token to '{token_path}': {e}")

    # Build the YouTube service client
    print("Building YouTube API service...")
    youtube = build("youtube", "v3", credentials=creds)

    total_files = len(validated_files)
    print(f"\nStarting upload of {total_files} file(s)...")

    for index, video_path in enumerate(validated_files, 1):
        # Prepare video title from filename
        video_filename = os.path.basename(video_path)
        title, _ = os.path.splitext(video_filename)

        print(f"\n[{index}/{total_files}] Preparing upload of video:")
        print(f"  File Path:   {video_path}")
        print(f"  Title:       {title}")
        print(f"  Category:    Music (ID 10)")
        print(f"  Description: None")
        print(f"  Playlist:    None")

        body = {
            "snippet": {
                "title": title,
                "description": "",
                "categoryId": "10" # Music
            },
            "status": {
                "privacyStatus": "public" # public by default
            }
        }

        # Setup media upload
        # Using 1MB chunk size (must be a multiple of 256KB)
        media = MediaFileUpload(video_path, chunksize=1024*1024, resumable=True)

        insert_request = youtube.videos().insert(
            part="snippet,status",
            body=body,
            media_body=media
        )

        # Resumable upload loop
        response = None
        retry = 0
        max_retries = 10

        print("Uploading video...")
        while response is None:
            try:
                status, response = insert_request.next_chunk()
                if status:
                    print(f"Upload Progress: {int(status.progress() * 100)}%")
            except googleapiclient.errors.HttpError as e:
                if e.resp.status in [500, 502, 503, 504]:
                    retry += 1
                    if retry > max_retries:
                        print(f"Error: Upload failed after {max_retries} retries due to server errors.")
                        raise e
                    sleep_seconds = retry * 5
                    print(f"Server error: {e.resp.status}. Retrying chunk upload in {sleep_seconds} seconds...")
                    time.sleep(sleep_seconds)
                else:
                    print(f"An unexpected API error occurred: {e}")
                    raise e
            except Exception as e:
                print(f"An unexpected error occurred during upload: {e}")
                raise e

        if response and "id" in response:
            print(f"Successfully uploaded: '{title}'!")
            print(f"Video ID: {response['id']}")
            print(f"URL:      https://www.youtube.com/watch?v={response['id']}")
        else:
            print(f"Upload finished, but unexpected response received: {response}")

    print("\nAll uploads completed successfully!")

if __name__ == "__main__":
    main()
