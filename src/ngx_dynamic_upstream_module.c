
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_dynamic_upstream_op.h"


static ngx_http_upstream_srv_conf_t *
ngx_dynamic_upstream_get_zone(ngx_http_request_t *r, ngx_dynamic_upstream_op_t *op);


static ngx_int_t
ngx_dynamic_upstream_create_response_buf(ngx_http_request_t *r, ngx_http_upstream_rr_peers_t *peers, ngx_buf_t *b, size_t size, ngx_int_t verbose);

static ngx_int_t
ngx_dynamic_upstream_backup_create_response_buf(ngx_http_request_t *r, ngx_http_upstream_rr_peers_t *peers, ngx_buf_t *b, size_t size, ngx_int_t verbose);


static ngx_int_t
ngx_dynamic_upstream_zones_response_buf(ngx_http_request_t *r, ngx_http_upstream_main_conf_t *umcf, ngx_buf_t *b, size_t size);

static ngx_int_t
ngx_dynamic_upstream_zones_buf_size(ngx_http_upstream_main_conf_t *umcf);

static ngx_int_t
ngx_dynamic_upstream_handler(ngx_http_request_t *r);
static char *
ngx_dynamic_upstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);


static ngx_command_t ngx_dynamic_upstream_commands[] = {
    {
        ngx_string("dynamic_upstream"),
        NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
        ngx_dynamic_upstream,
        0,
        0,
        NULL
    },

    ngx_null_command
};


static ngx_http_module_t ngx_dynamic_upstream_module_ctx = {
    NULL,                              /* preconfiguration */
    NULL,                              /* postconfiguration */

    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */

    NULL,                              /* create server configuration */
    NULL,                              /* merge server configuration */

    NULL,                              /* create location configuration */
    NULL                               /* merge location configuration */
};


ngx_module_t ngx_dynamic_upstream_module = {
    NGX_MODULE_V1,
    &ngx_dynamic_upstream_module_ctx, /* module context */
    ngx_dynamic_upstream_commands,    /* module directives */
    NGX_HTTP_MODULE,                  /* module type */
    NULL,                             /* init master */
    NULL,                             /* init module */
    NULL,                             /* init process */
    NULL,                             /* init thread */
    NULL,                             /* exit thread */
    NULL,                             /* exit process */
    NULL,                             /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_upstream_srv_conf_t *
ngx_dynamic_upstream_get_zone(ngx_http_request_t *r, ngx_dynamic_upstream_op_t *op)
{
    ngx_uint_t                      i;
    ngx_http_upstream_srv_conf_t   *uscf, **uscfp;
    ngx_http_upstream_main_conf_t  *umcf;

    umcf  = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);
    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        uscf = uscfp[i];
        if (uscf->shm_zone != NULL &&
            uscf->shm_zone->shm.name.len == op->upstream.len &&
            ngx_strncmp(uscf->shm_zone->shm.name.data, op->upstream.data, op->upstream.len) == 0)
        {
            return uscf;
        }
    }

    return NULL;
}

static ngx_int_t
ngx_dynamic_upstream_create_response_buf(ngx_http_request_t *r, ngx_http_upstream_rr_peers_t *peers,
    ngx_buf_t *b,
    size_t size,
    ngx_int_t verbose)
{
    ngx_http_upstream_rr_peer_t  *peer;
    u_char                        namebuf[512], *last;
    size_t                        idx;

    idx = 0;
    last = b->last + size;

    if (peers->name->len > 511) {
        return NGX_ERROR;
    }

    // upstream name
    ngx_cpystrn(namebuf, peers->name->data, peers->name->len + 1);

    b->last = ngx_snprintf(b->last, last - b->last, "{\"upstream\":\"%s\",\"backup\":0,\"number\":%d,\"server\":[", namebuf, peers->number );
    for (peer = peers->peer; peer; peer = peer->next) {
        if (peer->name.len > 511) {
            return NGX_ERROR;
        }
        ngx_cpystrn(namebuf, peer->name.data, peer->name.len + 1);
        if (verbose) {
            if (idx == 0) {
                b->last = ngx_snprintf(b->last, last - b->last, "{\"server\":\"%s\",\"weight\":%d,\"max_fails\":%d,\"fail_timeout\":%d,\"down\":%d}",
                                                       namebuf, peer->weight, peer->max_fails, peer->fail_timeout, peer->down);
            } else {
                b->last = ngx_snprintf(b->last, last - b->last, ",{\"server\":\"%s\",\"weight\":%d,\"max_fails\":%d,\"fail_timeout\":%d,\"down\":%d}",
                                                       namebuf, peer->weight, peer->max_fails, peer->fail_timeout, peer->down);
            }
        } else {
            if (idx == 0) {
                b->last = ngx_snprintf(b->last, last - b->last, "{\"server\":\"%s\",\"down\":%d}", namebuf, peer->down);
            } else {
                b->last = ngx_snprintf(b->last, last - b->last, ",{\"server\":\"%s\",\"down\":%d}", namebuf, peer->down);
            }
        }
        idx++;
    }
    b->last = ngx_snprintf(b->last, last - b->last, "]}");
    return NGX_OK;
}

// 获取backup服务器列表
static ngx_int_t
ngx_dynamic_upstream_backup_create_response_buf(ngx_http_request_t *r, ngx_http_upstream_rr_peers_t *peers, ngx_buf_t *b, size_t size, ngx_int_t verbose){
    ngx_http_upstream_rr_peer_t  *peer;
    u_char                        namebuf[512], *last;
    size_t                        idx;

    idx = 0;
    last = b->last + size;

    if (peers->name->len > 511) {
        return NGX_ERROR;
    }

    ngx_cpystrn(namebuf, peers->name->data, peers->name->len + 1);
    b->last = ngx_snprintf(b->last, last - b->last, "{\"upstream\":\"%s\",\"backup\":1,\"number\":%d,\"server\":[", namebuf, peers->next->number );

    // backup 在peers -> next 里面
    for (peer = peers->next->peer; peer; peer = peer->next) {
        if (peer->name.len > 511) {
            return NGX_ERROR;
        }
        ngx_cpystrn(namebuf, peer->name.data, peer->name.len + 1);
        if (idx == 0) {
            b->last = ngx_snprintf(b->last, last - b->last, "{\"server\":\"%s\",\"weight\":%d,\"max_fails\":%d,\"fail_timeout\":%d,\"down\":%d}",
                                                   namebuf, peer->weight, peer->max_fails, peer->fail_timeout, peer->down);
        } else {
            b->last = ngx_snprintf(b->last, last - b->last, ",{\"server\":\"%s\",\"weight\":%d,\"max_fails\":%d,\"fail_timeout\":%d,\"down\":%d}",
                                                   namebuf, peer->weight, peer->max_fails, peer->fail_timeout, peer->down);
        }
        idx++;
    }
    b->last = ngx_snprintf(b->last, last - b->last, "]}");
    return NGX_OK;
}

static ngx_int_t
ngx_dynamic_upstream_zones_buf_size(ngx_http_upstream_main_conf_t *umcf){
    ngx_http_upstream_srv_conf_t   *uscf, **uscfp;
    size_t                         i,size;
    size = 0;
    uscfp = umcf->upstreams.elts;
    for (i = 0; i < umcf->upstreams.nelts; i++) {
        uscf = uscfp[i];
        if (uscf->shm_zone != NULL) {
            if (uscf->shm_zone->shm.size) {
                size = size + uscf->shm_zone->shm.size;
            }
        }
    }
    return size;
}

static ngx_int_t
ngx_dynamic_upstream_zones_response_buf(ngx_http_request_t *r, ngx_http_upstream_main_conf_t *umcf, ngx_buf_t *b, size_t size)
{

    ngx_http_upstream_srv_conf_t   *uscf, **uscfp;
    u_char                        namebuf[512], *last;
    size_t                        i, idx;
    last = b->last + size;
    uscfp = umcf->upstreams.elts;

    idx = 0;
    b->last = ngx_snprintf(b->last, last - b->last, "{\"zones\":[");
    for (i = 0; i < umcf->upstreams.nelts; i++) {
        uscf = uscfp[i];
        if (uscf->shm_zone != NULL) {
            if (uscf->shm_zone->shm.name.len > 511) {
                return NGX_ERROR;
            }
            ngx_cpystrn(namebuf, uscf->shm_zone->shm.name.data, uscf->shm_zone->shm.name.len + 1);
            if (idx == 0) {
                b->last = ngx_snprintf(b->last, last - b->last, "\"%s\"", namebuf);
            } else {
                b->last = ngx_snprintf(b->last, last - b->last, ",\"%s\"", namebuf);
            }
            idx++;
        }
    }
    b->last = ngx_snprintf(b->last, last - b->last, "],\"number\":%d}",idx);
    return NGX_OK;
}

static ngx_int_t
ngx_dynamic_upstream_handler(ngx_http_request_t *r)
{
    size_t                          size;
    ngx_int_t                       rc;
    ngx_chain_t                     out;
    ngx_dynamic_upstream_op_t       op;
    ngx_buf_t                      *b;
    ngx_http_upstream_srv_conf_t   *uscf;
    ngx_slab_pool_t                *shpool;
    ngx_http_upstream_main_conf_t  *umcf;
    
    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }
    
    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_lowcase = NULL;

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    rc = ngx_dynamic_upstream_build_op(r, &op);
    if (rc != NGX_OK) {
        if (op.status == NGX_HTTP_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        return op.status;
    }
    // 只显示zone列表
    if(op.zones == 1) {
        // 添加自定义逻辑
        umcf  = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);
        size = ngx_dynamic_upstream_zones_buf_size(umcf);

        if (size == 0) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "upstream zone is not found. %s:%d",
                          __FUNCTION__,
                          __LINE__);
            return NGX_HTTP_NOT_FOUND;
        }

        b = ngx_create_temp_buf(r->pool, size);
        if (b == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        out.buf = b;
        out.next = NULL;
        rc = ngx_dynamic_upstream_zones_response_buf(r, (ngx_http_upstream_main_conf_t *)umcf, b, size);
        if (rc == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "failed to create a response. %s:%d",
                          __FUNCTION__,
                          __LINE__);
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        r->headers_out.status = NGX_HTTP_OK;
        r->headers_out.content_length_n = b->last - b->pos;

        b->last_buf = (r == r->main) ? 1 : 0;
        b->last_in_chain = 1;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }

        return ngx_http_output_filter(r, &out);
    }

    uscf = ngx_dynamic_upstream_get_zone(r, &op);
    if (uscf == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream is not found. %s:%d",
                      __FUNCTION__,
                      __LINE__);
        return NGX_HTTP_NOT_FOUND;
    }

    shpool = (ngx_slab_pool_t *) uscf->shm_zone->shm.addr;
    ngx_shmtx_lock(&shpool->mutex);
    
    rc = ngx_dynamic_upstream_op(r, &op, shpool, uscf);
    if (rc != NGX_OK) {
        ngx_shmtx_unlock(&shpool->mutex);
        if (op.status == NGX_HTTP_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        return op.status;
    }

    ngx_shmtx_unlock(&shpool->mutex);

    size = uscf->shm_zone->shm.size;

    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf = b;
    out.next = NULL;

    if ( op.server_list == 1 ) {
        rc = ngx_dynamic_upstream_create_response_buf(r, (ngx_http_upstream_rr_peers_t *)uscf->peer.data, b, size, op.verbose);
    } else {
        rc = ngx_dynamic_upstream_backup_create_response_buf(r, (ngx_http_upstream_rr_peers_t *)uscf->peer.data, b, size, op.verbose);
    }

    if (rc == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "failed to create a response. %s:%d",
                      __FUNCTION__,
                      __LINE__);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


static char *
ngx_dynamic_upstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_dynamic_upstream_handler;

    return NGX_CONF_OK;
}
