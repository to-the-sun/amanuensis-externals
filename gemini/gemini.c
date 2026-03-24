#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_systhread.h"
#include "ext_critical.h"
#include "ext_atomarray.h"
#include "ext_obstring.h"
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

typedef struct _gemini {
    t_object x_obj;
    void *x_outlet;
    t_symbol *apikey;
    t_symbol *model;

    // Threading and async management
    t_systhread x_thread;
    t_systhread_mutex x_mutex;
    t_qelem *x_qelem;

    char *pending_query;
    char *response_text;
    int thread_active;

    // WinHTTP Handles for cancellation
    HINTERNET hSession;
    HINTERNET hConnect;
    HINTERNET hRequest;
} t_gemini;

void *gemini_new(t_symbol *s, long argc, t_atom *argv);
void gemini_free(t_gemini *x);
void gemini_anything(t_gemini *x, t_symbol *s, long argc, t_atom *argv);
void gemini_assist(t_gemini *x, void *b, long m, long a, char *s);
void gemini_qtask(t_gemini *x);
void *gemini_threadproc(t_gemini *x);

static t_class *gemini_class;

// Helper function to escape JSON strings
static char *json_escape(const char *input) {
    if (!input) return NULL;
    size_t len = strlen(input);
    char *output = sysmem_newptr(len * 6 + 1); // Worst case: every character is escaped
    if (!output) return NULL;

    char *p = output;
    for (size_t i = 0; i < len; i++) {
        switch (input[i]) {
            case '\"': *p++ = '\\'; *p++ = '\"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b'; break;
            case '\f': *p++ = '\\'; *p++ = 'f'; break;
            case '\n': *p++ = '\\'; *p++ = 'n'; break;
            case '\r': *p++ = '\\'; *p++ = 'r'; break;
            case '\t': *p++ = '\\'; *p++ = 't'; break;
            default:
                if ((unsigned char)input[i] < 32) {
                    p += sprintf(p, "\\u%04x", (unsigned char)input[i]);
                } else {
                    *p++ = input[i];
                }
                break;
        }
    }
    *p = '\0';
    return output;
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("gemini", (method)gemini_new, (method)gemini_free, sizeof(t_gemini), 0L, A_GIMME, 0);

    class_addmethod(c, (method)gemini_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)gemini_assist, "assist", A_CANT, 0);

    CLASS_ATTR_SYM(c, "apikey", 0, t_gemini, apikey);
    CLASS_ATTR_LABEL(c, "apikey", 0, "API Key");

    CLASS_ATTR_SYM(c, "model", 0, t_gemini, model);
    CLASS_ATTR_LABEL(c, "model", 0, "Model Name");
    CLASS_ATTR_DEFAULT_SAVE(c, "model", 0, "gemini-1.5-flash");

    class_register(CLASS_BOX, c);
    gemini_class = c;
}

void *gemini_new(t_symbol *s, long argc, t_atom *argv) {
    t_gemini *x = (t_gemini *)object_alloc(gemini_class);

    if (x) {
        x->x_outlet = outlet_new(x, NULL);
        x->apikey = _sym_nothing;
        x->model = gensym("gemini-1.5-flash");
        x->pending_query = NULL;
        x->response_text = NULL;
        x->thread_active = 0;
        x->hSession = x->hConnect = x->hRequest = NULL;

        systhread_mutex_new(&x->x_mutex, 0);
        x->x_qelem = qelem_new(x, (method)gemini_qtask);

        attr_args_process(x, argc, argv);
    }
    return x;
}

void gemini_free(t_gemini *x) {
    qelem_free(x->x_qelem);

    // Abort any active connection
    systhread_mutex_lock(x->x_mutex);
    if (x->hRequest) WinHttpCloseHandle(x->hRequest);
    if (x->hConnect) WinHttpCloseHandle(x->hConnect);
    if (x->hSession) WinHttpCloseHandle(x->hSession);
    x->hRequest = x->hConnect = x->hSession = NULL;

    // Wait for thread if active
    if (x->thread_active) {
        systhread_mutex_unlock(x->x_mutex);
        unsigned int ret;
        systhread_join(x->x_thread, &ret);
    } else {
        systhread_mutex_unlock(x->x_mutex);
    }

    systhread_mutex_free(x->x_mutex);

    if (x->pending_query) sysmem_freeptr(x->pending_query);
    if (x->response_text) sysmem_freeptr(x->response_text);
}

void gemini_assist(t_gemini *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Query (symbol/list)");
    } else {
        sprintf(s, "Response (symbol)");
    }
}

void gemini_anything(t_gemini *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->apikey == _sym_nothing) {
        object_error((t_object *)x, "API key not set. Use @apikey attribute.");
        return;
    }

    systhread_mutex_lock(x->x_mutex);
    if (x->thread_active) {
        object_error((t_object *)x, "A query is already in progress.");
        systhread_mutex_unlock(x->x_mutex);
        return;
    }
    systhread_mutex_unlock(x->x_mutex);

    t_string *query_str = string_new(NULL);
    if (!query_str) return;

    // Concatenate everything into a query string
    if (s != _sym_nothing && s != gensym("list")) {
        string_append(query_str, s->s_name);
        string_append(query_str, " ");
    }

    for (int i = 0; i < argc; i++) {
        long textsize = 0;
        char *text = NULL;
        if (atom_gettext(1, argv + i, &textsize, &text, OBEX_UTIL_ATOM_GETTEXT_SYM_NO_QUOTE) == MAX_ERR_NONE && text) {
            string_append(query_str, text);
            string_append(query_str, " ");
            sysmem_freeptr(text);
        }
    }

    if (x->pending_query) sysmem_freeptr(x->pending_query);
    const char *final_query = string_getptr(query_str);
    x->pending_query = sysmem_newptr(strlen(final_query) + 1);
    strcpy(x->pending_query, final_query);
    object_free(query_str);

    systhread_mutex_lock(x->x_mutex);
    x->thread_active = 1;
    systhread_mutex_unlock(x->x_mutex);

    object_post((t_object *)x, "Initiating query: %s", x->pending_query);
    systhread_create((method)gemini_threadproc, x, 0, 0, 0, &x->x_thread);
}

void gemini_qtask(t_gemini *x) {
    systhread_mutex_lock(x->x_mutex);
    if (x->response_text) {
        outlet_anything(x->x_outlet, gensym(x->response_text), 0, NULL);
        sysmem_freeptr(x->response_text);
        x->response_text = NULL;
    }
    x->thread_active = 0;
    systhread_mutex_unlock(x->x_mutex);
}

void *gemini_threadproc(t_gemini *x) {
    BOOL bResults = FALSE;
    DWORD dwSize = 0;
    DWORD dwStatusCode = 0;
    DWORD dwDownloaded = 0;
    LPSTR pszOutBuffer;
    char *full_response = NULL;
    size_t full_response_len = 0;
    char *escaped_query = json_escape(x->pending_query);
    char *payload = NULL;

    if (!escaped_query) {
        systhread_mutex_lock(x->x_mutex);
        x->response_text = sysmem_newptr(32);
        strcpy(x->response_text, "Memory Error");
        systhread_mutex_unlock(x->x_mutex);
        qelem_set(x->x_qelem);
        return NULL;
    }

    payload = sysmem_newptr(strlen(escaped_query) + 128);
    sprintf(payload, "{\"contents\": [{\"parts\":[{\"text\": \"%s\"}]}]}", escaped_query);
    sysmem_freeptr(escaped_query);

    // Build URL with API Key
    wchar_t wUrl[1024];
    swprintf(wUrl, 1024, L"/v1beta/models/%hs:generateContent?key=%hs",
        x->model->s_name, x->apikey->s_name);

    systhread_mutex_lock(x->x_mutex);
    x->hSession = WinHttpOpen(L"Max Gemini Object/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);

    if (x->hSession)
        x->hConnect = WinHttpConnect(x->hSession, L"generativelanguage.googleapis.com",
                                  INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (x->hConnect)
        x->hRequest = WinHttpOpenRequest(x->hConnect, L"POST", wUrl,
                                      NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE);
    systhread_mutex_unlock(x->x_mutex);

    if (x->hRequest)
        bResults = WinHttpSendRequest(x->hRequest,
                                      L"Content-Type: application/json\r\n",
                                      (DWORD)-1L,
                                      (LPVOID)payload,
                                      (DWORD)strlen(payload),
                                      (DWORD)strlen(payload),
                                      0);

    if (payload) sysmem_freeptr(payload);

    if (bResults)
        bResults = WinHttpReceiveResponse(x->hRequest, NULL);

    if (bResults) {
        dwSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(x->hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

        if (dwStatusCode != 200) {
            char status_msg[64];
            snprintf(status_msg, 64, "HTTP Error %ld", dwStatusCode);
            full_response = sysmem_newptr(strlen(status_msg) + 1);
            strcpy(full_response, status_msg);
            object_error((t_object *)x, "Gemini API returned HTTP status %ld", dwStatusCode);
        }
    } else {
        DWORD err = GetLastError();
        object_error((t_object *)x, "WinHTTP Request failed with error %ld", err);
    }

    if (bResults && dwStatusCode == 200) {
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(x->hRequest, &dwSize)) break;

            pszOutBuffer = sysmem_newptr(dwSize + 1);
            if (!pszOutBuffer) break;

            ZeroMemory(pszOutBuffer, dwSize + 1);
            if (!WinHttpReadData(x->hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
                sysmem_freeptr(pszOutBuffer);
                break;
            }

            // Append to full_response
            char *new_response = sysmem_newptr(full_response_len + dwDownloaded + 1);
            if (full_response) {
                memcpy(new_response, full_response, full_response_len);
                sysmem_freeptr(full_response);
            }
            memcpy(new_response + full_response_len, pszOutBuffer, dwDownloaded);
            full_response_len += dwDownloaded;
            new_response[full_response_len] = '\0';
            full_response = new_response;

            sysmem_freeptr(pszOutBuffer);
        } while (dwSize > 0);
    }

    systhread_mutex_lock(x->x_mutex);
    if (x->hRequest) { WinHttpCloseHandle(x->hRequest); x->hRequest = NULL; }
    if (x->hConnect) { WinHttpCloseHandle(x->hConnect); x->hConnect = NULL; }
    if (x->hSession) { WinHttpCloseHandle(x->hSession); x->hSession = NULL; }
    systhread_mutex_unlock(x->x_mutex);

    // Parse JSON response using Max's dictionary
    char *final_text = NULL;
    if (full_response) {
        if (dwStatusCode != 200) {
            final_text = full_response; // Already contains error string
            full_response = NULL;
        } else {
            t_dictionary *d = NULL;
            t_max_err err = dictobj_dictionaryfromstring(&d, full_response, 1, NULL);
            if (err == MAX_ERR_NONE && d) {
            // Gemini response structure: candidates[0].content.parts[0].text
            t_atomarray *candidates = NULL;
            if (dictionary_getatomarray(d, gensym("candidates"), (t_object **)&candidates) == MAX_ERR_NONE && candidates) {
                t_atom cand_atom;
                if (atomarray_getindex(candidates, 0, &cand_atom) == MAX_ERR_NONE) {
                    t_dictionary *cand_dict = (t_dictionary *)atom_getobj(&cand_atom);
                    t_dictionary *content_dict = NULL;
                    if (dictionary_getdictionary(cand_dict, gensym("content"), (t_object **)&content_dict) == MAX_ERR_NONE && content_dict) {
                        t_atomarray *parts = NULL;
                        if (dictionary_getatomarray(content_dict, gensym("parts"), (t_object **)&parts) == MAX_ERR_NONE && parts) {
                            t_atom part_atom;
                            if (atomarray_getindex(parts, 0, &part_atom) == MAX_ERR_NONE) {
                                t_dictionary *part_dict = (t_dictionary *)atom_getobj(&part_atom);
                                t_symbol *text_sym = NULL;
                                if (dictionary_getsym(part_dict, gensym("text"), &text_sym) == MAX_ERR_NONE) {
                                    final_text = sysmem_newptr(strlen(text_sym->s_name) + 1);
                                    strcpy(final_text, text_sym->s_name);
                                    object_post((t_object *)x, "Successfully received response.");
                                }
                            }
                        }
                    }
                }
            }
                object_free(d);
            } else {
                final_text = sysmem_newptr(strlen("Error parsing response") + 1);
                strcpy(final_text, "Error parsing response");
                object_error((t_object *)x, "Failed to parse JSON response: %s", full_response);
            }
            sysmem_freeptr(full_response);
        }
    } else {
        final_text = sysmem_newptr(strlen("No response from server") + 1);
        strcpy(final_text, "No response from server");
    }

    systhread_mutex_lock(x->x_mutex);
    x->response_text = final_text;
    systhread_mutex_unlock(x->x_mutex);

    qelem_set(x->x_qelem);

    return NULL;
}
