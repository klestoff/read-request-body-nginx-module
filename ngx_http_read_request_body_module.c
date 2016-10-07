#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
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
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
    }

#if defined(nginx_version) && nginx_version >= 8011
    r->main->count--;
#endif

    if (!ctx->done) {
        ctx->done = 1;

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
        ctx = ngx_palloc(r->connection->pool, sizeof(ngx_http_read_request_body_ctx_t));
        if (ctx == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ctx->done = 0;

        ngx_http_set_ctx(r, ctx, ngx_http_read_request_body_module);
    }

    if (!ctx->done) {
        r->request_body_in_single_buf = 1;
        r->request_body_in_persistent_file = 1;
        r->request_body_in_clean_file = 1;

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
