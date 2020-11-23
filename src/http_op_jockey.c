#include "http_op_jockey.h"

#include <async/drystream.h>
#include <fstrace.h>

#include <errno.h>

struct http_op_response {
    const http_env_t *envelope;
    byte_array_t *body;
};

const http_env_t *http_op_response_get_envelope(http_op_response_t *response)
{
    return response->envelope;
}

byte_array_t *http_op_response_release_body(http_op_response_t *response)
{
    byte_array_t *body = response->body;
    response->body = NULL;
    return body;
}

json_thing_t *http_op_response_release_body_as_json(
    http_op_response_t *response)
{
    json_thing_t *body = NULL;
    if (response->body) {
        body = json_utf8_decode(byte_array_data(response->body),
                                byte_array_size(response->body));
        destroy_byte_array(response->body);
        response->body = NULL;
    }
    return body;
}

typedef enum {
    HTTP_OP_JOCKEY_READING_HEADERS,
    HTTP_OP_JOCKEY_READING_BODY,
    HTTP_OP_JOCKEY_DONE,
    HTTP_OP_JOCKEY_FAILED,
    HTTP_OP_JOCKEY_ZOMBIE,
} http_op_jockey_state_t;

struct http_op_jockey {
    async_t *async;
    http_op_t *op;
    uint64_t uid;
    http_op_jockey_state_t state;
    int error;
    bytestream_1 content;
    http_op_response_t response;
    action_1 callback;
};

FSTRACE_DECL(ASYNCHTTP_OP_JOCKEY_CREATE, "UID=%64u OP=%p");

http_op_jockey_t *make_http_op_jockey(async_t *async,
                                      http_op_t *op,
                                      size_t max_body_size)
{
    http_op_jockey_t *jockey = fscalloc(1, sizeof *jockey);
    jockey->async = async;
    jockey->uid = fstrace_get_unique_id();
    jockey->state = HTTP_OP_JOCKEY_READING_HEADERS;
    jockey->op = op;
    jockey->content = drystream;
    jockey->response.body = make_byte_array(max_body_size);
    FSTRACE(ASYNCHTTP_OP_JOCKEY_CREATE, jockey->uid, op);
    return jockey;
}

FSTRACE_DECL(ASYNCHTTP_OP_JOCKEY_DESTROY, "UID=%64u");

void http_op_jockey_close(http_op_jockey_t *jockey)
{
    FSTRACE(ASYNCHTTP_OP_JOCKEY_DESTROY, jockey->uid);
    if (jockey->response.body)
        destroy_byte_array(jockey->response.body);
    bytestream_1_close(jockey->content);
    http_op_close(jockey->op);
    jockey->state = HTTP_OP_JOCKEY_ZOMBIE;
    async_wound(jockey->async, jockey);
}

void http_op_jockey_register_callback(http_op_jockey_t *jockey, action_1 action)
{
    jockey->callback = action;
    http_op_register_callback(jockey->op, action);
    bytestream_1_register_callback(jockey->content, action);
}

void http_op_jockey_unregister_callback(http_op_jockey_t *jockey)
{
    jockey->callback = NULL_ACTION_1;
    http_op_unregister_callback(jockey->op);
    bytestream_1_unregister_callback(jockey->content);
}

static const char *trace_state(void *state)
{
    switch (*(http_op_jockey_state_t *) state) {
        case HTTP_OP_JOCKEY_READING_HEADERS:
            return "HTTP_OP_JOCKEY_READING_HEADERS";
        case HTTP_OP_JOCKEY_READING_BODY:
            return "HTTP_OP_JOCKEY_READING_BODY";
        case HTTP_OP_JOCKEY_DONE:
            return "HTTP_OP_JOCKEY_DONE";
        case HTTP_OP_JOCKEY_FAILED:
            return "HTTP_OP_JOCKEY_FAILED";
        case HTTP_OP_JOCKEY_ZOMBIE:
            return "HTTP_OP_JOCKEY_ZOMBIE";
        default:
            return "?";
    }
}

FSTRACE_DECL(ASYNCHTTP_OP_JOCKEY_SET_STATE, "UID=%64u OLD=%I NEW=%I");

static void set_state(http_op_jockey_t *jockey, http_op_jockey_state_t state)
{
    FSTRACE(ASYNCHTTP_OP_JOCKEY_SET_STATE,
            jockey->uid,
            trace_state,
            &jockey->state,
            trace_state,
            &state);
    jockey->state = state;
}

static ssize_t read_frame(void *obj, void *buf, size_t count)
{
    bytestream_1 *stream = obj;
    return bytestream_1_read(*stream, buf, count);
}

FSTRACE_DECL(ASYNCHTTP_OP_JOCKEY_READ_CONTENT_FAIL, "UID=%64u ERROR=%e");

static void probe_body(http_op_jockey_t *jockey)
{
    ssize_t count = byte_array_append_stream(jockey->response.body,
                                             read_frame,
                                             &jockey->content,
                                             2048);
    if (count < 0 && errno == ENOSPC) {
        char c;
        count = bytestream_1_read(jockey->content, &c, 1);
        if (count > 0) {
            jockey->error = EMSGSIZE;
            set_state(jockey, HTTP_OP_JOCKEY_FAILED);
            return;
        }
    }
    if (count < 0) {
        if (errno != EAGAIN) {
            FSTRACE(ASYNCHTTP_OP_JOCKEY_READ_CONTENT_FAIL, jockey->uid);
            jockey->error = errno;
            set_state(jockey, HTTP_OP_JOCKEY_FAILED);
        }
        return;
    }
    if (count == 0) {
        set_state(jockey, HTTP_OP_JOCKEY_DONE);
        return;
    }
    async_execute(jockey->async, jockey->callback);
    errno = EAGAIN;
}

FSTRACE_DECL(ASYNCHTTP_OP_JOCKEY_RECEIVE_RESP_FAIL, "UID=%64u ERROR=%e");
FSTRACE_DECL(ASYNCHTTP_OP_JOCKEY_RECEIVE_RESP_EOF, "UID=%64u");
FSTRACE_DECL(ASYNCHTTP_OP_JOCKEY_GOT_RESPONSE,
             "UID=%64u RESP=%d EXPLANATION=%s");
FSTRACE_DECL(ASYNCHTTP_OP_JOCKEY_GET_RESP_CONTENT_FAIL, "UID=%64u ERROR=%e");

static void probe_headers(http_op_jockey_t *jockey)
{
    errno = 0;
    const http_env_t *envelope = http_op_receive_response(jockey->op);
    if (!envelope) {
        if (errno) {
            if (errno == EAGAIN)
                return;
            FSTRACE(ASYNCHTTP_OP_JOCKEY_RECEIVE_RESP_FAIL, jockey->uid);
        } else {
            FSTRACE(ASYNCHTTP_OP_JOCKEY_RECEIVE_RESP_EOF, jockey->uid);
        }
        jockey->error = errno;
        set_state(jockey, HTTP_OP_JOCKEY_FAILED);
        return;
    }
    FSTRACE(ASYNCHTTP_OP_JOCKEY_GOT_RESPONSE,
            jockey->uid,
            http_env_get_code(jockey->response.envelope),
            http_env_get_explanation(envelope));
    if (http_op_get_response_content(jockey->op, &jockey->content) < 0) {
        FSTRACE(ASYNCHTTP_OP_JOCKEY_GET_RESP_CONTENT_FAIL, jockey->uid);
        jockey->error = errno;
        set_state(jockey, HTTP_OP_JOCKEY_FAILED);
        return;
    }
    jockey->response.envelope = envelope;
    set_state(jockey, HTTP_OP_JOCKEY_READING_BODY);
    bytestream_1_register_callback(jockey->content, jockey->callback);
    async_execute(jockey->async, jockey->callback);
    errno = EAGAIN;
}

http_op_response_t *http_op_jockey_receive_response(http_op_jockey_t *jockey)
{
    switch (jockey->state) {
        case HTTP_OP_JOCKEY_READING_HEADERS:
            probe_headers(jockey);
            break;
        case HTTP_OP_JOCKEY_READING_BODY:
            probe_body(jockey);
            break;
        default:
            break;
    }
    switch (jockey->state) {
        case HTTP_OP_JOCKEY_READING_HEADERS:
            return NULL;
        case HTTP_OP_JOCKEY_READING_BODY:
            return NULL;
        case HTTP_OP_JOCKEY_FAILED:
            errno = jockey->error;
            return NULL;
        case HTTP_OP_JOCKEY_DONE:
            return &jockey->response;
        default:
            errno = EINVAL;
            return NULL;
    }
}
