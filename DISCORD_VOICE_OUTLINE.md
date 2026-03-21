# Discord Audio Object Design Outline (`discord_voice~`)

This document outlines the architecture and implementation strategy for a Max/MSP external object (`discord_voice~`) capable of sending real-time audio from Max to a Discord voice channel.

## 1. Architecture Overview

The object will consist of three primary components working in tandem:

1.  **Signaling (WebSocket):** Manages the connection to the Discord Gateway and the Discord Voice Gateway. This handles authentication, joining voice channels, and negotiating encryption keys/UDP endpoints.
2.  **Audio Processing (DSP):** Captures incoming audio from the Max DSP chain, encodes it into the Opus format, and encapsulates it for transmission.
3.  **Transmission (UDP):** Sends the encrypted Opus packets to Discord's voice servers via UDP.

### Threading Model

-   **Main Thread:** Handles Max messages (e.g., `connect`, `join`), configuration, and the WebSocket state machine.
-   **DSP Thread:** Collects audio samples. To maintain real-time safety, encoding and network I/O should be offloaded to a dedicated high-priority background thread to avoid stalling the audio engine.
-   **Worker Thread(s):** A `t_systhread` will be used to manage the Opus encoding and UDP socket transmission, consuming audio from a FIFO buffer populated by the DSP thread.

## 2. Protocol Implementation

### A. Discord Gateway (WebSocket)
-   Identify/Resume to the main Discord Gateway.
-   Receive `VOICE_STATE_UPDATE` and `VOICE_SERVER_UPDATE` events to get the session ID and endpoint.

### B. Discord Voice Gateway (WebSocket)
-   Establish a second WebSocket connection to the voice server provided.
-   Perform Opcode 0 (Identify) with the server's token.
-   Negotiate protocols (UDP) and encryption modes.

### C. Audio Encoding (Opus)
-   Discord requires 48kHz, 2-channel (stereo) Opus audio.
-   Max audio (typically float) must be converted to PCM16 or Float32 and passed to `libopus`.

### D. Encryption (libsodium / libdave)
-   Discord uses AEAD (AES-256-GCM or XChaCha20-Poly1305) for voice packet encryption.
-   `libsodium` is the standard library for these operations. Newer implementations may require `libdave` for end-to-end encryption support.

### E. UDP Transmission (RTP)
-   Construct RTP-like headers for each Opus packet.
-   Perform IP discovery via UDP if necessary.
-   Send packets at a consistent 20ms interval (960 samples at 48kHz).

## 3. Required Dependencies

For a Windows (64-bit) build using MinGW-w64:

-   **`libopus`:** For audio encoding.
-   **`libsodium`:** For encryption and security.
-   **`libwebsockets`** (or a lightweight C WebSocket library): For gateway signaling.
-   **`Winsock2` (`ws2_32`):** For underlying UDP and TCP networking (already used in `shared/visualize.c`).

## 4. Implementation Steps

1.  **Skeleton:** Create the Max external boilerplate with inlets for audio and a control inlet for commands (`connect`, `join`).
2.  **Networking Core:** Implement a basic WebSocket client and UDP socket handler using `Winsock2`.
3.  **Signaling Logic:** Implement the state machine for the Discord/Voice Gateway handshake.
4.  **Audio Pipeline:** Implement the FIFO buffer between the DSP thread and the worker thread.
5.  **Integration:** Integrate `libopus` for encoding and `libsodium` for packet encryption.
6.  **Optimization:** Tune the buffer sizes and thread priorities for low-latency, stable transmission.

## 5. Potential Challenges

-   **Library Linking:** Ensuring `libopus` and `libsodium` are correctly cross-compiled or linked for Windows within the provided environment.
-   **Real-time Stability:** Minimizing jitter in the UDP transmission to prevent audio artifacts in Discord.
-   **Discord API Changes:** Keeping up with Discord's evolving voice protocol (e.g., the transition to the DAVE protocol).
