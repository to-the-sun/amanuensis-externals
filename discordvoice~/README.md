# discordvoice~

A Max/MSP external for streaming audio to Discord voice channels.

## Current Status: Functional Skeleton
This object implements the core networking and signaling infrastructure required by the Discord Voice Protocol (v4). While it can successfully connect to Discord and join a voice channel, it currently lacks native audio encoding.

### Implemented Features
- **Gateway Signaling:** Connects to Discord Gateway (v10) for authentication and guild/channel events.
- **Voice Gateway Handshake:** Implements the Voice Gateway (v4) handshake, including `Identify`, `Select Protocol`, and `Speaking` states.
- **IP Discovery:** Automatic UDP port discovery for RTP transmission.
- **Security:** Standard RTP encryption using AES-256-GCM (AEAD_AES256_GCM_RTPSIZE) via Windows BCrypt.
- **Multi-threaded Architecture:** Decoupled signaling (WS) and high-priority audio (UDP) threads.

### Current Limitations
- **No Native Opus Encoding:** The object currently transmits silent Opus frames or random noise (in `test_tone` mode). To stream real audio, `libopus` must be integrated into the `discordvoice_audio_thread_proc` routine.
- **DAVE (E2EE) Protocol:** The object announces compatibility with DAVE version 1, but does not yet implement the full MLS (Messaging Layer Security) handshake required for channels with mandatory End-to-End Encryption.
- **Platform:** Currently supports 64-bit Windows only (uses WinHTTP and BCrypt).

## Usage
1. Instantiate `discordvoice~`.
2. Enable logging with `@log 1`.
3. Send the `connect` message: `connect [bot_token] [guild_id] [channel_id]`.
4. Monitor the status outlet (State 5 indicates Voice Ready).

## Future Integration Steps
To enable full audio streaming:
1. **Opus:** Link against `libopus`. In `discordvoice_audio_thread_proc`, use `opus_encode` to compress the 20ms PCM frames from the ring buffer into the `opus_encoded` buffer before calling `discordvoice_send_audio_packet`.
2. **DAVE:** Implement the `SESSION_DESCRIPTION` extensions for MLS key exchange if Discord reports a non-zero `dave_version`.
