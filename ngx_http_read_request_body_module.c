#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <zlib.h>

typedef struct {
    ngx_chain_t *bufs;
    unsigned done:1;
} ngx_http_read_request_body_ctx_t;

typedef struct {
    ngx_flag_t read_request_body;
} ngx_http_read_request_body_conf_t;

static ngx_int_t
ngx_http_read_request_body_init(ngx_conf_t *cf);

static void *
ngx_http_read_request_body_create_cf(ngx_conf_t *cf);

static char *
ngx_http_read_request_body_merge_cf(ngx_conf_t *cf, void *parent, void *child);

static char *
ngx_http_read_request_body(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t
decompress(ngx_http_request_t *, ngx_http_read_request_body_ctx_t *);
static ngx_table_elt_t *content_encoding(const ngx_list_t *hdrs);

static ngx_int_t
gunzip_body(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx);
static ngx_int_t
inflate_body(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx);
static ngx_int_t
decomp(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx, int code);
static ngx_int_t
decomp_body(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx,
        z_stream *, char *buf, size_t size);
static ngx_chain_t *cpy(ngx_http_request_t *, const char *buf, size_t size);

static ngx_command_t ngx_http_read_request_body_module_commands[] = {
    {
        ngx_string("read_request_body"),
        NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_NOARGS,
        ngx_http_read_request_body,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },

    ngx_null_command
};

static ngx_http_module_t ngx_http_read_request_body_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_read_request_body_init,       /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_read_request_body_create_cf,  /* create location configuration */
    ngx_http_read_request_body_merge_cf    /* merge location configuration */
};

ngx_module_t ngx_http_read_request_body_module = {
    NGX_MODULE_V1,
    &ngx_http_read_request_body_module_ctx,       /* module context */
    ngx_http_read_request_body_module_commands,   /* module directives */
    NGX_HTTP_MODULE,                              /* module type */
    NULL,                                         /* init master */
    NULL,                                         /* init module */
    NULL,                                         /* init process */
    NULL,                                         /* init thread */
    NULL,                                         /* exit thread */
    NULL,                                         /* exit process */
    NULL,                                         /* exit master */
    NGX_MODULE_V1_PADDING
};

ngx_module_t *ngx_modules[] = {
	&ngx_http_read_request_body_module,
	NULL
};

char *ngx_module_names[] = {
	"ngx_http_read_request_body_module",
	NULL
};

char *ngx_module_order[] = {
	NULL
};

static size_t
body_size(ngx_http_request_t *r) {
    ngx_http_core_loc_conf_t  *clcf;

    if (!r->headers_in.chunked) {
        return r->headers_in.content_length_n;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    if (clcf->client_max_body_size) {
        return clcf->client_max_body_size;
    }

    return 1024 * 1024; /* 1 MiB */
}

static ngx_int_t
save(ngx_chain_t *bufs, ngx_http_request_t *r) {
    ngx_http_request_body_t *rb;
    ngx_int_t                bytes = 0;

    rb = r->request_body;
    if (rb) {
        ngx_chain_t *cl;
        ngx_buf_t   *src, *dst;
        size_t       n, available;

        dst = bufs->buf;
        for (cl = rb->busy; cl; cl = cl->next) {
            src = cl->buf;
            n = src->last - src->pos;
            available = dst->end - dst->last;
            if (n > available) {
                ngx_buf_t *tmp;

                size_t already = dst->last - dst->pos;
                size_t required = already + n;
                size_t size = 2 * (dst->end - dst->start);
                if (required > size) {
                    size = required;
                }
                tmp = ngx_create_temp_buf(r->pool, size);
                if (!tmp) {
                    ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                                  "Oom reallocating request body buffer");
                    return NGX_ERROR;
                }
                memcpy(tmp->pos, dst->pos, already);
                tmp->last += already;
                dst = tmp;
            }
            memcpy(dst->last, src->pos, n);
            src->pos += n;
            dst->last += n;

            bytes += n;
        }
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "read request "
                   "body module: %d bytes", bytes);

    return bytes;
}

static void
do_read(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx) {
    ngx_int_t rc, bytes;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, __FUNCTION__);

    do {
        rc = ngx_http_read_unbuffered_request_body(r);

        bytes = save(ctx->bufs, r);
        if (bytes < 0 || rc == NGX_ERROR) {
            r->read_event_handler = ngx_http_block_reading;
            ctx->done = 1;
            r->request_body->bufs = ctx->bufs;
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }

        if (rc == NGX_OK) {
            r->read_event_handler = ngx_http_block_reading;
            ctx->done = 1;
            rc = decompress(r, ctx);
            if (rc != NGX_OK) {
                // TODO
            }
            r->request_body->bufs = ctx->bufs;
#if defined(nginx_version) && nginx_version >= 8011
            r->main->count--;
#endif
            ngx_http_core_run_phases(r);
            return;
        }

        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            r->read_event_handler = ngx_http_block_reading;
            ctx->done = 1;
            r->request_body->bufs = ctx->bufs;
            ngx_http_finalize_request(r, rc);
            return;
        }
    } while (bytes > 0);
}

static void
on_read(ngx_http_request_t *r) {
    ngx_http_read_request_body_ctx_t *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_read_request_body_module);
    if (ctx == NULL) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "The request context of the ngx_http_read_request_body "
                      "module is null");
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    do_read(r, ctx);
}

static void
ngx_http_read_request_body_post_handler(ngx_http_request_t *r)
{
    ngx_http_read_request_body_ctx_t  *ctx;

    ngx_log_debug0(
        NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "finalize read request body"
    );

    ctx = ngx_http_get_module_ctx(r, ngx_http_read_request_body_module);

    if (ctx == NULL) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "The request context of the ngx_http_read_request_body "
                      "module is null");
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (r->reading_body) {
        ngx_buf_t *b;
        size_t     size;

        size = body_size(r);
        b = ngx_create_temp_buf(r->pool, size);
        if (!b) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                          "Oom allocating request body buffer");
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }
        ctx->bufs = ngx_alloc_chain_link(r->pool);
        if (!ctx->bufs) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                          "Oom allocating request body buffer chain");
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }
        ctx->bufs->next = NULL;
        ctx->bufs->buf = b;

        if (save(ctx->bufs, r) < 0) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }

        r->read_event_handler = on_read;
        do_read(r, ctx);
    } else {
        ngx_int_t rc;

        ctx->done = 1;
        rc = decompress(r, ctx);
        if (rc != NGX_OK) {
            // TODO
        }

#if defined(nginx_version) && nginx_version >= 8011
        r->main->count--;
#endif

        ngx_http_core_run_phases(r);
    }
}

static ngx_int_t
ngx_http_read_request_body_handler(ngx_http_request_t *r)
{
    ngx_int_t                          rc;
    ngx_http_read_request_body_conf_t *rrbcf;
    ngx_http_read_request_body_ctx_t  *ctx;

    ngx_log_debug2(
        NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "read request body rewrite handler, uri:\"%V\" c:%ud",
        &r->uri, r->main->count
    );

    rrbcf = ngx_http_get_module_loc_conf(r, ngx_http_read_request_body_module);

    if (!rrbcf->read_request_body) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "read_request_body not being used");

        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_read_request_body_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->connection->pool, sizeof(ngx_http_read_request_body_ctx_t));
        if (ctx == NULL) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                          "Oom allocating ngx_http_read_request_body context");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_read_request_body_module);
    }

    if (!ctx->done) {
        r->request_body_no_buffering = 1;

        rc = ngx_http_read_client_request_body(r, ngx_http_read_request_body_post_handler);

        if (rc == NGX_ERROR) {
            return rc;
        }

        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
#if (nginx_version < 1002006) ||                                             \
        (nginx_version >= 1003000 && nginx_version < 1003009)
            r->main->count--;

#endif
            return rc;
        }

        return NGX_DONE;
    }

    return NGX_DECLINED;
}

static ngx_int_t
ngx_http_read_request_body_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt               *h;
    ngx_http_core_main_conf_t         *cmcf;
    ngx_http_read_request_body_conf_t *rrbcf;

    rrbcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_read_request_body_module);

    if (!rrbcf->read_request_body) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0, "read_request_body not being used");

        return NGX_OK;
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_read_request_body_handler;

    return NGX_OK;
}

static void *
ngx_http_read_request_body_create_cf(ngx_conf_t *cf)
{
    ngx_http_read_request_body_conf_t *rrbcf;

    rrbcf = ngx_palloc(cf->pool, sizeof(ngx_http_read_request_body_conf_t));
    if (rrbcf == NULL) {
        return NGX_CONF_ERROR;
    }

    rrbcf->read_request_body = NGX_CONF_UNSET;

    return rrbcf;
}

static char *
ngx_http_read_request_body_merge_cf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_read_request_body_conf_t *prev = parent;
    ngx_http_read_request_body_conf_t *conf = child;

    ngx_conf_merge_value(conf->read_request_body, prev->read_request_body, 0);

    return NGX_CONF_OK;
}


static char *
ngx_http_read_request_body(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_read_request_body_conf_t *rrbcf = conf;

    rrbcf->read_request_body = 1;

    return NGX_CONF_OK;
}

static ngx_int_t
decompress(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx)
{
    static const char GZIP[] = "gzip";
    static const char DEFLATE[] = "deflate";

    ngx_table_elt_t *e = content_encoding(&r->headers_in.headers);
    if (e == NULL) {
        return NGX_OK;
    }

    const ngx_str_t *h = &e->value;
    if (h->len == sizeof(GZIP) - 1 &&
        ngx_strncmp(h->data, GZIP, sizeof(GZIP) - 1) == 0) {
        ngx_int_t rc = gunzip_body(r, ctx);
        if (rc == NGX_OK) {
            e->hash = 0;
        }
        return rc;
    }
    if (h->len == sizeof(DEFLATE) - 1 &&
        ngx_strncmp(h->data, DEFLATE, sizeof(DEFLATE) - 1) == 0) {
        ngx_int_t rc = inflate_body(r, ctx);
        if (rc == NGX_OK) {
            e->hash = 0;
        }
        return rc;
    }
    return NGX_OK;
}

static ngx_table_elt_t *
content_encoding(const ngx_list_t *hdrs)
{
    static const char HDR[] = "content-encoding";

    const ngx_list_part_t *part = &hdrs->part;
    do {
        ngx_uint_t i;
        for (i = 0; i < part->nelts; ++i) {
            ngx_table_elt_t *e = (ngx_table_elt_t *)part->elts + i;
            if (e->hash && e->key.len == sizeof(HDR) - 1 &&
                ngx_strncmp(e->lowcase_key, HDR, sizeof(HDR) - 1) == 0) {
                return e;
            }
        }
        part = part->next;
    } while (part != NULL);
    return NULL;
}

static ngx_int_t
gunzip_body(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx)
{
    return decomp(r, ctx, 31);
}

static ngx_int_t
inflate_body(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx)
{
    return decomp(r, ctx, -15);
}

static ngx_int_t
decomp(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx, int code)
{
    static const size_t buf_size = 1 << 15;

    ngx_int_t rc;
    z_stream zstream;
    char *buf = ngx_palloc(r->pool, buf_size);
    if (!buf) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                      "Oom allocating decompression buffer");
        return NGX_ERROR;
    }
    memset(&zstream, 0, sizeof(zstream));
    if (inflateInit2(&zstream, code) != Z_OK) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                      "Error initializing zstream");
        return NGX_ERROR;
    }

    rc = decomp_body(r, ctx, &zstream, buf, buf_size);

    inflateEnd(&zstream);

    return rc;
}

static ngx_int_t
decomp_body(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx,
            z_stream *z, char *buf, size_t size)
{
    ngx_chain_t out, *c, *last;

    out.next = NULL;
    last = &out;
    for (c = ctx->bufs; c; c = c->next) {
        ngx_buf_t *b = c->buf;
        z->next_in = (Bytef *)b->pos;
        z->avail_in = ngx_buf_size(b);
        int rc;
        do {
            z->next_out  = (Bytef *)buf;
            z->avail_out = size;
            rc = inflate(z, 0);
            if (rc != Z_OK && rc != Z_STREAM_END) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, NGX_EINVAL,
                              "Invalid compressed data");
                return NGX_ERROR;
            }
            // copy [buf, buf + size - z->avail_out) to buffer
            if (z->avail_out < size) {
                ngx_chain_t *ch = cpy(r, buf, size - z->avail_out);
                if (!ch) {
                    return NGX_ERROR;
                }
                last->next = ch;
                last = ch;
            }
        } while (rc == Z_OK && z->avail_in > 0);
        // rc == Z_STREAM_END || rc == Z_OK && z->avail_in <= 0
    }

    z->next_in = NULL;
    z->avail_in = 0;
    int rc;
    do {
        z->next_out  = (Bytef *)buf;
        z->avail_out = size;
        rc = inflate(z, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, NGX_EINVAL,
                          "Invalid compressed data");
            return NGX_ERROR;
        }
        // copy [buf, buf + size - z->avail_out) to buffer
        if (z->avail_out < size) {
            ngx_chain_t *ch = cpy(r, buf, size - z->avail_out);
            if (!ch) {
                return NGX_ERROR;
            }
            last->next = ch;
            last = ch;
        }
    } while (rc == Z_OK);
    // rc == Z_STREAM_END

    if (out.next) {
        last->buf->last_buf = 1;
        last->buf->last_in_chain = 1;

        ctx->bufs = out.next;
    }

    return NGX_OK;
}

static ngx_chain_t *
cpy(ngx_http_request_t *r, const char *buf, size_t size)
{
    ngx_chain_t *c;
    ngx_buf_t *b;

    c = ngx_alloc_chain_link(r->pool);
    if (!c) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                      "Oom allocating buffer chain");
        return NULL;
    }
    b = ngx_create_temp_buf(r->pool, size);
    if (!b) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                      "Oom allocating temporary buffer");
        return NULL;
    }

    memcpy(b->pos, buf, size);
    b->last = b->pos + size;

    c->buf  = b;
    c->next = NULL;

    return c;
}
