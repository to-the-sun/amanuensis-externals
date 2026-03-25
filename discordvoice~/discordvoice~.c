#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "z_dsp.h"
#include "ext_systhread.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <bcrypt.h>
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "../shared/logging.h"
#include <string.h>
#include <stdio.h>

/**
 * @file discordvoice~.c
 * @brief Max external for sending audio to Discord voice channels.
 * 
 * This object is a skeleton for implementing Discord's voice protocol within Max.
 */

typedef struct _discordvoice {
    t_pxobject d_obj;
    t_systhread thread;
    t_critical lock;
    int terminate;
    void *status_outlet;
    void *log_outlet;
    long log;

    t_symbol *token;
    t_symbol *guild_id;
    t_symbol *channel_id;
    t_symbol *user_id;

    // Networking handles (using void* to avoid header conflicts in struct)
    void *h_session;
    void *h_connect;
    void *h_request;
    void *h_websocket;

    void *h_v_connect;
    void *h_v_request;
    void *h_v_websocket;

    long heartbeat_interval;
    long v_heartbeat_interval;
    DWORD v_last_heartbeat_tick;
    DWORD last_heartbeat_tick;
    long last_sequence;
    int connected;
    int identified;
    int ready;
    int v_ready;
    int heartbeat_acked;

    t_symbol *voice_token;
    t_symbol *voice_endpoint;
    t_symbol *session_id;

    // UDP Voice Connection
    SOCKET udp_sock;
    struct sockaddr_in v_server_addr;
    char my_ip[64];
    int my_port;
    unsigned int ssrc;
    unsigned short sequence;
    unsigned int timestamp;
    unsigned char secret_key[32];
    t_symbol *v_mode;

    // Audio Buffer
    float *audio_buffer;
    int buffer_head;
    int buffer_tail;
    int buffer_size;

    // WebSocket receive buffers
    BYTE *recv_buffer;
    DWORD recv_buffer_pos;
    BYTE *v_recv_buffer;
    DWORD v_recv_buffer_pos;

    // Encryption (BCrypt)
    BCRYPT_ALG_HANDLE hAesAlg;
    BCRYPT_KEY_HANDLE hAesKey;
    int aes_initialized;

    DWORD last_audio_tick;
} t_discordvoice;

void *discordvoice_new(t_symbol *s, long argc, t_atom *argv);
void discordvoice_free(t_discordvoice *x);
void discordvoice_assist(t_discordvoice *x, void *b, long m, long a, char *s);
void discordvoice_bang(t_discordvoice *x);
void discordvoice_connect(t_discordvoice *x, t_symbol *s, long argc, t_atom *argv);
void *discordvoice_thread_proc(t_discordvoice *x);
void discordvoice_dsp64(t_discordvoice *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void discordvoice_perform64(t_discordvoice *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void discordvoice_log(t_discordvoice *x, const char *fmt, ...);

static t_class *discordvoice_class;

void discordvoice_log(t_discordvoice *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "discordvoice~", fmt, args);
    va_end(args);
    
    // Always post errors/critical info to the Max console regardless of @log attribute
    if (strstr(fmt, "failed") || strstr(fmt, "Error") || strstr(fmt, "terminated")) {
        char buf[1024];
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        object_error((t_object *)x, "discordvoice~: %s", buf);
    }
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("discordvoice~", (method)discordvoice_new, (method)discordvoice_free, sizeof(t_discordvoice), 0L, A_GIMME, 0);

    class_addmethod(c, (method)discordvoice_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)discordvoice_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)discordvoice_bang, "bang", 0);
    class_addmethod(c, (method)discordvoice_connect, "connect", A_GIMME, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_discordvoice, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    discordvoice_class = c;
}

void *discordvoice_new(t_symbol *s, long argc, t_atom *argv) {
    t_discordvoice *x = (t_discordvoice *)object_alloc(discordvoice_class);

    if (x) {
        x->log = 0;
        x->terminate = 0;
        x->thread = NULL;
        x->connected = 0;
        x->identified = 0;
        x->token = _sym_nothing;
        x->guild_id = _sym_nothing;
        x->channel_id = _sym_nothing;
        x->user_id = _sym_nothing;

        x->h_session = NULL;
        x->h_connect = NULL;
        x->h_request = NULL;
        x->h_websocket = NULL;

    x->h_v_connect = NULL;
    x->h_v_request = NULL;
    x->h_v_websocket = NULL;

        x->voice_token = _sym_nothing;
        x->voice_endpoint = _sym_nothing;
        x->session_id = _sym_nothing;

        x->udp_sock = INVALID_SOCKET;
        x->my_port = 0;
        x->ssrc = 0;
        x->sequence = 0;
        x->timestamp = 0;
        memset(x->secret_key, 0, 32);
        x->v_mode = _sym_nothing;

        x->buffer_size = 48000 * 2; // 1 second of stereo
        x->audio_buffer = (float *)sysmem_newptr(x->buffer_size * sizeof(float));
        x->buffer_head = 0;
        x->buffer_tail = 0;
        x->last_audio_tick = 0;

        x->recv_buffer = (BYTE *)sysmem_newptr(65536);
        x->recv_buffer_pos = 0;
        x->v_recv_buffer = (BYTE *)sysmem_newptr(65536);
        x->v_recv_buffer_pos = 0;

        x->hAesAlg = NULL;
        x->hAesKey = NULL;
        x->aes_initialized = 0;

        attr_args_process(x, argc, argv);

        dsp_setup((t_pxobject *)x, 2); // 2 signal inlets (L/R)
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->status_outlet = outlet_new((t_object *)x, NULL);
        
        critical_new(&x->lock);
        
        discordvoice_log(x, "initialized");
    }
    return x;
}

void discordvoice_free(t_discordvoice *x) {
    dsp_free((t_pxobject *)x);
    
    if (x->audio_buffer) sysmem_freeptr(x->audio_buffer);
    if (x->recv_buffer) sysmem_freeptr(x->recv_buffer);
    if (x->v_recv_buffer) sysmem_freeptr(x->v_recv_buffer);

    if (x->hAesKey) BCryptDestroyKey(x->hAesKey);
    if (x->hAesAlg) BCryptCloseAlgorithmProvider(x->hAesAlg, 0);

    critical_enter(x->lock);
    x->terminate = 1;
    critical_exit(x->lock);

    if (x->thread) {
        unsigned int ret;
        systhread_join(x->thread, &ret);
        x->thread = NULL;
    }

    critical_free(x->lock);
}

void discordvoice_connect(t_discordvoice *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc < 3) {
        object_error((t_object *)x, "connect message requires 3 arguments: [token] [guild_id] [channel_id]");
        return;
    }

    // Robust symbol/ID conversion
    x->token = atom_getsym(argv);
    
    if (atom_gettype(argv + 1) == A_SYM) {
        x->guild_id = atom_getsym(argv + 1);
    } else {
        char buf[64];
        snprintf(buf, 64, "%lld", (long long)atom_getlong(argv + 1));
        x->guild_id = gensym(buf);
    }

    if (atom_gettype(argv + 2) == A_SYM) {
        x->channel_id = atom_getsym(argv + 2);
    } else {
        char buf[64];
        snprintf(buf, 64, "%lld", (long long)atom_getlong(argv + 2));
        x->channel_id = gensym(buf);
    }

    if (x->thread) {
        object_warn((t_object *)x, "already connecting/connected, restart not implemented yet");
        return;
    }

    x->terminate = 0;
    systhread_create((method)discordvoice_thread_proc, x, 0, 0, 0, &x->thread);
}

void discordvoice_send_heartbeat(t_discordvoice *x, HINTERNET hWebSocket) {
    char json[128];
    if (x->last_sequence >= 0) {
        snprintf(json, sizeof(json), "{\"op\":1,\"d\":%ld}", x->last_sequence);
    } else {
        snprintf(json, sizeof(json), "{\"op\":1,\"d\":null}");
    }
    WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json, (DWORD)strlen(json));
    x->last_heartbeat_tick = GetTickCount();
    discordvoice_log(x, "Sent Heartbeat: %s", json);
}

void discordvoice_send_identify(t_discordvoice *x, HINTERNET hWebSocket) {
    char json[1024];
    // Identify with intents 129 (GUILDS | GUILD_VOICE_STATES)
    // Use standard property names and remove trailing ~ which might cause issues
    snprintf(json, sizeof(json), 
        "{\"op\":2,\"d\":{\"token\":\"%s\",\"properties\":{\"os\":\"windows\",\"browser\":\"discordvoice\",\"device\":\"discordvoice\"},\"intents\":129}}",
        x->token->s_name);
    WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json, (DWORD)strlen(json));
    discordvoice_log(x, "Sent Identify: %s", json);
}

void discordvoice_send_voice_state_update(t_discordvoice *x, HINTERNET hWebSocket) {
    char json[1024];
    snprintf(json, sizeof(json), 
        "{\"op\":4,\"d\":{\"guild_id\":\"%s\",\"channel_id\":\"%s\",\"self_mute\":false,\"self_deaf\":false}}",
        x->guild_id->s_name, x->channel_id->s_name);
    WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json, (DWORD)strlen(json));
    discordvoice_log(x, "Sent Voice State Update: %s", json);
}

void discordvoice_send_v_heartbeat(t_discordvoice *x, HINTERNET hVWebSocket) {
    char json[128];
    snprintf(json, sizeof(json), "{\"op\":3,\"d\":%ld}", GetTickCount());
    WinHttpWebSocketSend(hVWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json, (DWORD)strlen(json));
    x->v_last_heartbeat_tick = GetTickCount();
    discordvoice_log(x, "Sent Voice Heartbeat: %s", json);
}

void discordvoice_send_v_identify(t_discordvoice *x, HINTERNET hVWebSocket) {
    char json[1024];
    snprintf(json, sizeof(json), 
        "{\"op\":0,\"d\":{\"server_id\":\"%s\",\"user_id\":\"%s\",\"session_id\":\"%s\",\"token\":\"%s\",\"dave_protocol_version\":1}}",
        x->guild_id->s_name, x->user_id->s_name, x->session_id->s_name, x->voice_token->s_name);
    WinHttpWebSocketSend(hVWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json, (DWORD)strlen(json));
    discordvoice_log(x, "Sent Voice Identify: %s", json);
}

void discordvoice_send_v_select_protocol(t_discordvoice *x, HINTERNET hVWebSocket) {
    char json[1024];
    snprintf(json, sizeof(json), 
        "{\"op\":1,\"d\":{\"protocol\":\"udp\",\"data\":{\"address\":\"%s\",\"port\":%d,\"mode\":\"aead_aes256_gcm_rtpsize\"}}}",
        x->my_ip, x->my_port);
    WinHttpWebSocketSend(hVWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json, (DWORD)strlen(json));
    discordvoice_log(x, "Sent Voice Select Protocol: %s:%d", x->my_ip, x->my_port);
}

void discordvoice_send_v_speaking(t_discordvoice *x, HINTERNET hVWebSocket, int speaking) {
    char json[256];
    snprintf(json, sizeof(json), "{\"op\":5,\"d\":{\"speaking\":%d,\"delay\":0,\"ssrc\":%u}}", speaking ? 5 : 0, x->ssrc);
    WinHttpWebSocketSend(hVWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json, (DWORD)strlen(json));
    discordvoice_log(x, "Sent Voice Speaking: %d", speaking);
}

void discordvoice_send_audio_packet(t_discordvoice *x, unsigned char *opus_data, int opus_len) {
    if (x->udp_sock == INVALID_SOCKET) return;
    if (!x->aes_initialized) return;

    unsigned char packet[1500];
    // RTP Header (12 bytes)
    packet[0] = 0x80;
    packet[1] = 0x78; // Opus
    *(unsigned short *)(packet + 2) = htons(x->sequence++);
    *(unsigned int *)(packet + 4) = htonl(x->timestamp);
    x->timestamp += 960; // 20ms at 48kHz
    *(unsigned int *)(packet + 8) = htonl(x->ssrc);

    // AEAD_AES256_GCM_RTPSIZE
    // Nonce is the 12-byte RTP header
    unsigned char nonce[12];
    memcpy(nonce, packet, 12);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO paddingInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(paddingInfo);
    unsigned char tag[16];
    paddingInfo.pbNonce = nonce;
    paddingInfo.cbNonce = 12;
    paddingInfo.pbTag = tag;
    paddingInfo.cbTag = 16;

    DWORD cbResult = 0;
    NTSTATUS status = BCryptEncrypt(x->hAesKey, opus_data, opus_len, &paddingInfo, NULL, 0, packet + 12, opus_len, &cbResult, 0);

    if (status == 0) {
        // Append auth tag
        memcpy(packet + 12 + opus_len, tag, 16);
        sendto(x->udp_sock, (const char *)packet, 12 + opus_len + 16, 0, (struct sockaddr *)&x->v_server_addr, sizeof(x->v_server_addr));
    } else {
        discordvoice_log(x, "BCryptEncrypt failed with status 0x%x", status);
    }
}

void *discordvoice_thread_proc(t_discordvoice *x) {
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL, hWebSocket = NULL;
    HINTERNET hVConnect = NULL, hVRequest = NULL, hVWebSocket = NULL;
    DWORD timeout = 1000; // 1 second timeout for polling
    
    discordvoice_log(x, "starting gateway connection thread");

    hSession = WinHttpOpen(L"discordvoice~/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        discordvoice_log(x, "WinHttpOpen failed (Error %u)", GetLastError());
        goto cleanup;
    }

    // Discord Gateway URL: wss://gateway.discord.gg/?v=10&encoding=json
    hConnect = WinHttpConnect(hSession, L"gateway.discord.gg", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        discordvoice_log(x, "WinHttpConnect failed (Error %u)", GetLastError());
        goto cleanup;
    }

    hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/?v=10&encoding=json", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        discordvoice_log(x, "WinHttpOpenRequest failed (Error %u)", GetLastError());
        goto cleanup;
    }

    WinHttpSetTimeouts(hSession, 0, 0, 0, (int)timeout);

    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        discordvoice_log(x, "WinHttpSetOption (Upgrade) failed (Error %u)", GetLastError());
        goto cleanup;
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        discordvoice_log(x, "WinHttpSendRequest failed (Error %u)", GetLastError());
        goto cleanup;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        discordvoice_log(x, "WinHttpReceiveResponse failed (Error %u)", GetLastError());
        goto cleanup;
    }

    // Check HTTP status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
        if (statusCode != 101) {
            discordvoice_log(x, "WebSocket upgrade failed with HTTP Status %u", statusCode);
            goto cleanup;
        }
    }

    hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    if (!hWebSocket) {
        discordvoice_log(x, "WinHttpWebSocketCompleteUpgrade failed (Error %u)", GetLastError());
        goto cleanup;
    }

    WinHttpCloseHandle(hRequest);
    hRequest = NULL;

    critical_enter(x->lock);
    x->h_session = (void *)hSession;
    x->h_connect = (void *)hConnect;
    x->h_websocket = (void *)hWebSocket;
    x->connected = 1;
    critical_exit(x->lock);

    discordvoice_log(x, "connected to gateway");

    x->heartbeat_interval = 0;
    x->v_heartbeat_interval = 0;
    x->last_sequence = -1;
    x->identified = 0;
    x->ready = 0;
        x->v_ready = 0;
    x->heartbeat_acked = 1;
    x->last_heartbeat_tick = GetTickCount();

    // Message loop
    x->recv_buffer_pos = 0;

    while (!x->terminate) {
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;
        BYTE chunk[8192];
        DWORD bytesRead = 0;
        DWORD err = WinHttpWebSocketReceive(hWebSocket, chunk, sizeof(chunk), &bytesRead, &bufferType);

        if (err == ERROR_SUCCESS) {
            if (x->recv_buffer_pos + bytesRead < 65536) {
                memcpy(x->recv_buffer + x->recv_buffer_pos, chunk, bytesRead);
                x->recv_buffer_pos += bytesRead;
            }

            if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                x->recv_buffer[x->recv_buffer_pos] = 0;
                // discordvoice_log(x, "Received: %s", (char *)x->recv_buffer);
                
                t_dictionary *d = NULL;
                char errstr[256];
                if (dictobj_dictionaryfromstring(&d, (char *)x->recv_buffer, 1, errstr) == MAX_ERR_NONE) {
                    t_atom_long op = -1;
                    dictionary_getlong(d, gensym("op"), &op);

                    if (op == 10) { // Hello
                        t_dictionary *data_dict = NULL;
                        if (dictionary_getdictionary(d, gensym("d"), (t_object **)&data_dict) == MAX_ERR_NONE) {
                            t_atom_long interval = 0;
                            dictionary_getlong(data_dict, gensym("heartbeat_interval"), &interval);
                            x->heartbeat_interval = (long)interval;
                            discordvoice_log(x, "Gateway Hello, Heartbeat Interval: %ld", x->heartbeat_interval);
                            
                            x->last_heartbeat_tick = GetTickCount();
                            // First heartbeat and identify should be sent immediately
                            discordvoice_send_heartbeat(x, hWebSocket);
                            discordvoice_send_identify(x, hWebSocket);
                            x->identified = 1;
                        }
                    } else if (op == 11) { // Heartbeat ACK
                        x->heartbeat_acked = 1;
                        discordvoice_log(x, "Heartbeat ACK received");
                    } else if (op == 1) { // Heartbeat Request
                        discordvoice_send_heartbeat(x, hWebSocket);
                    } else if (op == 0) { // Dispatch
                        t_symbol *t = NULL;
                        dictionary_getsym(d, gensym("t"), &t);
                        t_atom_long s = -1;
                        dictionary_getlong(d, gensym("s"), &s);
                        x->last_sequence = (long)s;

                        if (t == gensym("READY")) {
                            discordvoice_log(x, "Gateway READY");
                            x->ready = 1;

                            t_dictionary *data_dict = NULL;
                            if (dictionary_getdictionary(d, gensym("d"), (t_object **)&data_dict) == MAX_ERR_NONE) {
                                t_symbol *s_id = NULL;
                                dictionary_getsym(data_dict, gensym("session_id"), &s_id);
                                x->session_id = s_id;
                                discordvoice_log(x, "Session ID: %s", x->session_id->s_name);

                                t_dictionary *user_dict = NULL;
                                if (dictionary_getdictionary(data_dict, gensym("user"), (t_object **)&user_dict) == MAX_ERR_NONE) {
                                    t_symbol *u_id = NULL;
                                    dictionary_getsym(user_dict, gensym("id"), &u_id);
                                    x->user_id = u_id;
                                    discordvoice_log(x, "Bot User ID: %s", x->user_id->s_name);
                                }
                            }
                            discordvoice_send_voice_state_update(x, hWebSocket);
                        } else if (t == gensym("VOICE_STATE_UPDATE")) {
                             discordvoice_log(x, "VOICE_STATE_UPDATE received");
                        } else if (t == gensym("VOICE_SERVER_UPDATE")) {
                             discordvoice_log(x, "VOICE_SERVER_UPDATE received");
                             t_dictionary *data_dict = NULL;
                             if (dictionary_getdictionary(d, gensym("d"), (t_object **)&data_dict) == MAX_ERR_NONE) {
                                 t_symbol *v_token = NULL;
                                 t_symbol *v_endpoint = NULL;
                                 dictionary_getsym(data_dict, gensym("token"), &v_token);
                                 dictionary_getsym(data_dict, gensym("endpoint"), &v_endpoint);
                                 x->voice_token = v_token;
                                 x->voice_endpoint = v_endpoint;
                                 discordvoice_log(x, "Voice Token: %s", x->voice_token->s_name);
                                 discordvoice_log(x, "Voice Endpoint: %s", x->voice_endpoint->s_name);
                                 
                                 // Voice Gateway Connection
                                 WCHAR szVHost[256];
                                 char vhost[256];
                                 const char *pend = strstr(x->voice_endpoint->s_name, ":");
                                 if (pend) {
                                     size_t len = pend - x->voice_endpoint->s_name;
                                     strncpy(vhost, x->voice_endpoint->s_name, len);
                                     vhost[len] = 0;
                                 } else {
                                     strcpy(vhost, x->voice_endpoint->s_name);
                                 }
                                 MultiByteToWideChar(CP_UTF8, 0, vhost, -1, szVHost, 256);

                                 hVConnect = WinHttpConnect(hSession, szVHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
                                 if (hVConnect) {
                                     hVRequest = WinHttpOpenRequest(hVConnect, L"GET", L"/?v=4", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                                     if (hVRequest) {
                                         WinHttpSetOption(hVRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
                                         if (WinHttpSendRequest(hVRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                                             if (WinHttpReceiveResponse(hVRequest, NULL)) {
                                                 hVWebSocket = WinHttpWebSocketCompleteUpgrade(hVRequest, 0);
                                                 if (hVWebSocket) {
                                                     discordvoice_log(x, "Connected to Voice Gateway");
                                                     WinHttpCloseHandle(hVRequest);
                                                     hVRequest = NULL;
                                                     discordvoice_send_v_identify(x, hVWebSocket);
                                                 }
                                             }
                                         }
                                     }
                                 }
                             }
                        }
                    }
                    object_free(d);
                }
                x->recv_buffer_pos = 0;
            } else if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT closeStatus = 0;
                BYTE closeReason[128];
                DWORD closeReasonLength = 0;
                WinHttpWebSocketQueryCloseStatus(hWebSocket, &closeStatus, closeReason, sizeof(closeReason), &closeReasonLength);
                closeReason[closeReasonLength] = 0;
                discordvoice_log(x, "WebSocket closed by server (Status: %u, Reason: %s)", closeStatus, (char *)closeReason);
                break;
            }
        } else if (err == ERROR_WINHTTP_TIMEOUT || err == 12002) {
             // Timeout is fine, just loop
        } else {
            discordvoice_log(x, "WinHttpWebSocketReceive failed with error %d", err);
            break;
        }

        // Check for Voice Gateway messages
        if (hVWebSocket) {
            WINHTTP_WEB_SOCKET_BUFFER_TYPE vBufferType;
            BYTE vChunk[4096];
            DWORD vBytesRead = 0;

            DWORD vErr = WinHttpWebSocketReceive(hVWebSocket, vChunk, sizeof(vChunk), &vBytesRead, &vBufferType);
            if (vErr == ERROR_SUCCESS) {
                if (x->v_recv_buffer_pos + vBytesRead < 65536) {
                    memcpy(x->v_recv_buffer + x->v_recv_buffer_pos, vChunk, vBytesRead);
                    x->v_recv_buffer_pos += vBytesRead;
                }

                if (vBufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                    x->v_recv_buffer[x->v_recv_buffer_pos] = 0;
                    t_dictionary *vd = NULL;
                    char verrstr[256];
                    if (dictobj_dictionaryfromstring(&vd, (char *)x->v_recv_buffer, 1, verrstr) == MAX_ERR_NONE) {
                        t_atom_long vop = -1;
                        dictionary_getlong(vd, gensym("op"), &vop);
                        if (vop == 8) { // Hello
                            t_dictionary *vdata_dict = NULL;
                            if (dictionary_getdictionary(vd, gensym("d"), (t_object **)&vdata_dict) == MAX_ERR_NONE) {
                                t_atom_long vinterval = 0;
                                dictionary_getlong(vdata_dict, gensym("heartbeat_interval"), &vinterval);
                                x->v_heartbeat_interval = (long)vinterval;
                                x->v_last_heartbeat_tick = GetTickCount();
                                discordvoice_log(x, "Voice Hello, Heartbeat: %ld", x->v_heartbeat_interval);
                            }
                        } else if (vop == 2) { // READY
                            discordvoice_log(x, "Voice READY Payload received");
                            discordvoice_send_v_speaking(x, hVWebSocket, 1);
                            x->last_audio_tick = GetTickCount();
                            x->v_ready = 1;

                            t_dictionary *vdata_dict = NULL;
                            if (dictionary_getdictionary(vd, gensym("d"), (t_object **)&vdata_dict) == MAX_ERR_NONE) {
                                t_symbol *ip = NULL;
                                t_atom_long port = 0;
                                t_atom_long ssrc = 0;
                                dictionary_getsym(vdata_dict, gensym("ip"), &ip);
                                dictionary_getlong(vdata_dict, gensym("port"), &port);
                                dictionary_getlong(vdata_dict, gensym("ssrc"), &ssrc);
                                x->ssrc = (unsigned int)ssrc;

                                discordvoice_log(x, "Voice READY, SSRC: %u, Server UDP: %s:%ld", x->ssrc, ip->s_name, (long)port);

                                // Perform IP Discovery
                                x->udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                                if (x->udp_sock != INVALID_SOCKET) {
                                    memset(&x->v_server_addr, 0, sizeof(x->v_server_addr));
                                    x->v_server_addr.sin_family = AF_INET;
                                    x->v_server_addr.sin_port = htons((u_short)port);
                                    x->v_server_addr.sin_addr.S_un.S_addr = inet_addr(ip->s_name);

                                    unsigned char packet[74];
                                    memset(packet, 0, 74);
                                    packet[0] = 0x01; // Request
                                    packet[1] = 0x4a; // Message length
                                    *(unsigned int *)(packet + 4) = htonl(x->ssrc);

                                    sendto(x->udp_sock, (const char *)packet, 74, 0, (struct sockaddr *)&x->v_server_addr, sizeof(x->v_server_addr));
                                    
                                    struct sockaddr_in from;
                                    int fromlen = sizeof(from);
                                    int n = recvfrom(x->udp_sock, (char *)packet, 74, 0, (struct sockaddr *)&from, &fromlen);
                                    if (n >= 70) {
                                        strcpy(x->my_ip, (const char *)(packet + 8));
                                        x->my_port = ntohs(*(unsigned short *)(packet + 72));
                                        discordvoice_log(x, "IP Discovery Success: My IP is %s, Port %d", x->my_ip, x->my_port);
                                        
                                        discordvoice_send_v_select_protocol(x, hVWebSocket);
                                    } else {
                                        discordvoice_log(x, "IP Discovery Failed");
                                    }
                                }
                            }
                        } else if (vop == 4) { // SESSION_DESCRIPTION
                            t_dictionary *vdata_dict = NULL;
                            if (dictionary_getdictionary(vd, gensym("d"), (t_object **)&vdata_dict) == MAX_ERR_NONE) {
                                t_symbol *mode = NULL;
                                dictionary_getsym(vdata_dict, gensym("mode"), &mode);
                                x->v_mode = mode;
                                discordvoice_log(x, "Voice Session Description, Mode: %s", x->v_mode->s_name);
                                
                                t_atomarray *key_aa = NULL;
                                if (dictionary_getatomarray(vdata_dict, gensym("secret_key"), (t_object **)&key_aa) == MAX_ERR_NONE) {
                                    long key_len = 0;
                                    t_atom *key_atoms = NULL;
                                    atomarray_getatoms(key_aa, &key_len, &key_atoms);
                                    for (int i = 0; i < 32 && i < key_len; i++) {
                                        x->secret_key[i] = (unsigned char)atom_getlong(key_atoms + i);
                                    }
                                    discordvoice_log(x, "Voice Secret Key received");

                                    // Initialize BCrypt AES-GCM
                                    if (x->hAesKey) { BCryptDestroyKey(x->hAesKey); x->hAesKey = NULL; }
                                    if (!x->hAesAlg) {
                                        BCryptOpenAlgorithmProvider(&x->hAesAlg, BCRYPT_AES_ALGORITHM, NULL, 0);
                                        BCryptSetProperty(x->hAesAlg, BCRYPT_CHAINING_MODE, (PBYTE)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
                                    }
                                    
                                    // Generate key from secret_key
                                    BCryptGenerateSymmetricKey(x->hAesAlg, &x->hAesKey, NULL, 0, x->secret_key, 32, 0);
                                    x->aes_initialized = 1;
                                    discordvoice_log(x, "Voice Encryption initialized (AES-256-GCM)");
                                }
                            }
                        }
                        object_free(vd);
                    }
                    x->v_recv_buffer_pos = 0;
                } else if (vBufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                    USHORT vCloseStatus = 0;
                    BYTE vCloseReason[128];
                    DWORD vCloseReasonLength = 0;
                    WinHttpWebSocketQueryCloseStatus(hVWebSocket, &vCloseStatus, vCloseReason, sizeof(vCloseReason), &vCloseReasonLength);
                    vCloseReason[vCloseReasonLength] = 0;
                    discordvoice_log(x, "Voice WebSocket closed by server (Status: %u, Reason: %s)", (unsigned int)vCloseStatus, (char *)vCloseReason);
                    break;
                }
            }
        }

        // Check for heartbeats
        DWORD now = GetTickCount();
        if (x->heartbeat_interval > 0 && now - x->last_heartbeat_tick >= (DWORD)x->heartbeat_interval) {
            if (!x->heartbeat_acked) {
                discordvoice_log(x, "Heartbeat ACK not received, closing connection");
                break;
            }
            x->heartbeat_acked = 0;
            discordvoice_send_heartbeat(x, hWebSocket);
            if (!x->identified) {
                discordvoice_send_identify(x, hWebSocket);
                x->identified = 1;
            }
        }
        if (hVWebSocket && x->v_heartbeat_interval > 0 && now - x->v_last_heartbeat_tick >= (DWORD)x->v_heartbeat_interval) {
            discordvoice_send_v_heartbeat(x, hVWebSocket);
        }

        // Audio Transmission Loop (20ms)
        if (x->udp_sock != INVALID_SOCKET && x->v_ready && now - x->last_audio_tick >= 20) {
            int samples_needed = 960 * 2; // 20ms of stereo at 48kHz
            float frame[1920];
            int available = 0;
            
            critical_enter(x->lock);
            available = (x->buffer_head - x->buffer_tail + x->buffer_size) % x->buffer_size;
            if (available >= samples_needed) {
                for (int i = 0; i < samples_needed; i++) {
                    frame[i] = x->audio_buffer[x->buffer_tail];
                    x->buffer_tail = (x->buffer_tail + 1) % x->buffer_size;
                }
            }
            critical_exit(x->lock);

            if (available >= samples_needed) {
                // In a real implementation, we would encode frame[] with libopus here.
                // Since we cannot link libopus, we'll send a valid-looking Opus "silence" 
                // packet but the user will need to link the library to get real audio.
                unsigned char opus_packet[3] = {0xF8, 0xFF, 0xFE};
                discordvoice_send_audio_packet(x, opus_packet, 3);
            } else {
                // Not enough audio buffered yet, send silence to maintain timing
                unsigned char opus_silence[3] = {0xF8, 0xFF, 0xFE};
                discordvoice_send_audio_packet(x, opus_silence, 3);
            }
            x->last_audio_tick = now;
        }

        systhread_sleep(1);
    }

cleanup:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hWebSocket) WinHttpCloseHandle(hWebSocket);
    if (hVRequest) WinHttpCloseHandle(hVRequest);
    if (hVWebSocket) WinHttpCloseHandle(hVWebSocket);
    if (hVConnect) WinHttpCloseHandle(hVConnect);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    critical_enter(x->lock);
    x->connected = 0;
    x->h_websocket = NULL;
    x->h_connect = NULL;
    x->h_session = NULL;
    critical_exit(x->lock);

    object_post((t_object *)x, "discordvoice~: gateway connection thread terminated");
    return NULL;
}

void discordvoice_assist(t_discordvoice *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1 (signal): Left audio input / messages"); break;
            case 1: sprintf(s, "Inlet 2 (signal): Right audio input"); break;
        }
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (int): Connection status (0-4)"); break;
            case 1: sprintf(s, "Outlet 2 (anything): Logging and protocol status"); break;
        }
    }
}

void discordvoice_bang(t_discordvoice *x) {
    int status = 0;
    critical_enter(x->lock);
    if (x->v_ready) status = 5;
    else if (x->ready) status = 4;
    else if (x->identified) status = 3;
    else if (x->connected) status = 2;
    else if (x->thread) status = 1;
    critical_exit(x->lock);
    
    outlet_int(x->status_outlet, status);
}

void discordvoice_dsp64(t_discordvoice *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)discordvoice_perform64, 0, NULL);
}

void discordvoice_perform64(t_discordvoice *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *in_l = ins[0];
    double *in_r = ins[1];

    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        for (int i = 0; i < sampleframes; i++) {
            int next_head = (x->buffer_head + 2) % x->buffer_size;
            if (next_head != x->buffer_tail) {
                x->audio_buffer[x->buffer_head] = (float)in_l[i];
                x->audio_buffer[x->buffer_head + 1] = (float)in_r[i];
                x->buffer_head = next_head;
            }
        }
        critical_exit(x->lock);
    }
}
