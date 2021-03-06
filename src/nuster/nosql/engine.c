/*
 * nuster nosql engine functions.
 *
 * Copyright (C) Jiang Wenyuan, < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <inttypes.h>

#include <types/global.h>
#include <types/stream.h>
#include <types/channel.h>
#include <types/proxy.h>

#include <proto/stream_interface.h>
#include <proto/http_ana.h>
#include <proto/acl.h>
#include <proto/log.h>
#include <proto/proxy.h>
#include <proto/http_htx.h>
#include <common/htx.h>

#include <nuster/nuster.h>

static void
nst_nosql_handler(hpx_appctx_t *appctx) {
    hpx_stream_interface_t  *si      = appctx->owner;
    hpx_stream_t            *s       = si_strm(si);
    hpx_channel_t           *req     = si_oc(si);
    hpx_channel_t           *res     = si_ic(si);
    nst_ring_item_t         *item    = NULL;
    hpx_buffer_t            *buf;
    hpx_htx_t               *req_htx, *res_htx;
    hpx_htx_blk_type_t       type;
    hpx_htx_blk_t           *blk;
    char                    *p, *ptr;
    uint64_t                 offset, payload_len;
    uint32_t                 blksz, sz, info;
    int                      ret, max, fd, header_len, total;

    res_htx = htxbuf(&res->buf);
    total   = res_htx->data;

    if(unlikely(si->state == SI_ST_DIS || si->state == SI_ST_CLO)) {
        goto out;
    }

    /* Check if the input buffer is avalaible. */
    if(!b_size(&res->buf)) {
        si_rx_room_blk(si);

        goto out;
    }

    /* check that the output is not closed */
    if(res->flags & (CF_SHUTW|CF_SHUTW_NOW)) {
        appctx->st0 = NST_CTX_STATE_DONE;
    }

    switch(appctx->st0) {
        case NST_NOSQL_APPCTX_STATE_CREATE:

            if(co_data(req)) {
                req_htx = htx_from_buf(&req->buf);
                co_htx_skip(req, req_htx, co_data(req));
                htx_to_buf(req_htx, &req->buf);
            }

            break;
        case NST_NOSQL_APPCTX_STATE_HIT_MEMORY:

            if(appctx->ctx.nuster.store.ring.item) {
                item = appctx->ctx.nuster.store.ring.item;

                while(item) {

                    if(nst_http_ring_item_to_htx(item, res_htx) != NST_OK) {
                        si_rx_room_blk(si);

                        goto out;
                    }

                    item = item->next;
                }

            } else {

                if(!htx_add_endof(res_htx, HTX_BLK_EOM)) {
                    si_rx_room_blk(si);

                    goto out;
                }

                if(!(res->flags & CF_SHUTR) ) {
                    res->flags |= CF_READ_NULL;
                    si_shutr(si);
                }

                /* eat the whole request */
                if(co_data(req)) {
                    req_htx = htx_from_buf(&req->buf);
                    co_htx_skip(req, req_htx, co_data(req));
                    htx_to_buf(req_htx, &req->buf);
                }

                nst_ring_data_detach(&nuster.nosql->store.ring, appctx->ctx.nuster.store.ring.data);
            }

out:
            appctx->ctx.nuster.store.ring.item = item;
            total = res_htx->data - total;

            if(total) {
                channel_add_input(res, total);
            }

            htx_to_buf(res_htx, &res->buf);

            break;
        case NST_NOSQL_APPCTX_STATE_HIT_DISK:
            {
                max         = b_room(&res->buf) - global.tune.maxrewrite;
                header_len  = appctx->ctx.nuster.store.disk.header_len;
                payload_len = appctx->ctx.nuster.store.disk.payload_len;
                offset      = appctx->ctx.nuster.store.disk.offset;
                fd          = appctx->ctx.nuster.store.disk.fd;

                switch(appctx->st1) {
                    case NST_DISK_APPLET_HEADER:
                        buf = get_trash_chunk();
                        p   = buf->area;

                        ret = pread(fd, p, header_len, offset);

                        if(ret != header_len) {
                            appctx->st1 = NST_DISK_APPLET_ERROR;

                            break;
                        }

                        while(header_len != 0) {
                            hpx_htx_blk_type_t  type;
                            hpx_htx_blk_t      *blk;
                            char               *ptr;
                            uint32_t            blksz, sz, info;

                            info  = *(uint32_t *)p;
                            type  = (info >> 28);
                            blksz = (info & 0xff) + ((info >> 8) & 0xfffff);
                            blk   = htx_add_blk(res_htx, type, blksz);

                            if(!blk) {
                                appctx->st1 = NST_DISK_APPLET_ERROR;

                                break;
                            }

                            blk->info = info;

                            ptr = htx_get_blk_ptr(res_htx, blk);
                            sz  = htx_get_blksz(blk);
                            p  += 4;
                            memcpy(ptr, p, sz);
                            p  += sz;

                            header_len -= 4 + sz;
                        }

                        appctx->st1 = NST_DISK_APPLET_PAYLOAD;
                        offset += ret;
                        appctx->ctx.nuster.store.disk.offset += ret;

                        break;
                    case NST_DISK_APPLET_PAYLOAD:
                        buf = get_trash_chunk();
                        p   = buf->area;
                        max = htx_get_max_blksz(res_htx, channel_htx_recv_max(res, res_htx));

                        if(max <= 0) {
                            goto end;
                        }

                        if(max < payload_len) {
                            ret = pread(fd, p, max, offset);
                        } else {
                            ret = pread(fd, p, payload_len, offset);
                        }

                        if(ret <= 0) {
                            appctx->st1 = NST_DISK_APPLET_ERROR;

                            break;
                        }

                        appctx->ctx.nuster.store.disk.payload_len -= ret;

                        type  = HTX_BLK_DATA;
                        info  = (type << 28) + ret;
                        blksz = info & 0xfffffff;
                        blk   = htx_add_blk(res_htx, type, blksz);

                        if(!blk) {
                            appctx->st1 = NST_DISK_APPLET_ERROR;

                            break;
                        }

                        blk->info = info;

                        ptr = htx_get_blk_ptr(res_htx, blk);
                        sz  = htx_get_blksz(blk);
                        memcpy(ptr, p, sz);

                        offset += ret;
                        appctx->ctx.nuster.store.disk.offset = offset;

                        if(appctx->ctx.nuster.store.disk.payload_len == 0) {
                            appctx->st1 = NST_DISK_APPLET_EOP;
                        } else {
                            si_rx_room_blk(si);

                            break;
                        }

                    case NST_DISK_APPLET_EOP:

                        if(!htx_add_endof(res_htx, HTX_BLK_EOT)) {
                            si_rx_room_blk(si);

                            goto end;
                        }

                        if(!htx_add_endof(res_htx, HTX_BLK_EOM)) {
                            si_rx_room_blk(si);

                            goto end;
                        }

                        appctx->st1 = NST_DISK_APPLET_DONE;
                    case NST_DISK_APPLET_DONE:

                        close(fd);

                        if(!(res->flags & CF_SHUTR) ) {
                            res->flags |= CF_READ_NULL;
                            si_shutr(si);
                        }

                        if(co_data(req)) {
                            req_htx = htx_from_buf(&req->buf);
                            co_htx_skip(req, req_htx, co_data(req));
                            htx_to_buf(req_htx, &req->buf);
                        }

                        break;
                    case NST_DISK_APPLET_ERROR:
                        si_shutr(si);
                        res->flags |= CF_READ_NULL;
                        close(fd);

                        break;
                }
            }

end:
            total = res_htx->data - total;

            if(total) {
                channel_add_input(res, total);
            }

            htx_to_buf(res_htx, &res->buf);

            break;
        case NST_NOSQL_APPCTX_STATE_END:
            nst_http_reply(s, NST_HTTP_200);

            break;
        case NST_NOSQL_APPCTX_STATE_NOT_FOUND:
            nst_http_reply(s, NST_HTTP_404);

            break;
        case NST_NOSQL_APPCTX_STATE_ERROR:
            nst_http_reply(s, NST_HTTP_500);

            break;
        case NST_NOSQL_APPCTX_STATE_FULL:
            nst_http_reply(s, NST_HTTP_507);

            break;
        default:
            co_skip(si_oc(si), co_data(si_oc(si)));

            break;
    }

    return;
}

void
nst_nosql_housekeeping() {
    uint64_t  start;

    if(global.nuster.nosql.status == NST_STATUS_ON && master == 1) {

        int  dict_cleaner = global.nuster.nosql.dict_cleaner;
        int  data_cleaner = global.nuster.nosql.data_cleaner;
        int  disk_cleaner = global.nuster.nosql.disk_cleaner;
        int  disk_loader  = global.nuster.nosql.disk_loader;
        int  disk_saver   = global.nuster.nosql.disk_saver;
        int  ms           = 10;
        int  ratio        = 1;

        start = get_current_timestamp();

        while(dict_cleaner--) {
            nst_dict_cleanup(&nuster.nosql->dict);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

        start = get_current_timestamp();

        if(data_cleaner > nuster.nosql->store.ring.count) {
            data_cleaner = nuster.nosql->store.ring.count;
        }

        if(nuster.nosql->store.ring.count) {
            ratio = nuster.nosql->store.ring.invalid * 10 / nuster.nosql->store.ring.count;
        }

        if(ratio >= 2) {
            data_cleaner = nuster.nosql->store.ring.count;

            ms = ms * ratio ;
            ms = ms >= 100 ? 100 : ms;
        }

        while(data_cleaner--) {
            nst_ring_cleanup(&nuster.nosql->store.ring);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

        start = get_current_timestamp();
        ms    = 10;

        while(disk_cleaner--) {
            nst_disk_cleanup(nuster.nosql);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

        start = get_current_timestamp();

        while(disk_loader--) {
            nst_disk_load(nuster.nosql);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

        start = get_current_timestamp();

        while(disk_saver--) {
            nst_ring_store_sync(nuster.nosql);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }
    }
}

void
nst_nosql_init() {
    nuster.applet.nosql.fct = nst_nosql_handler;

    if(global.nuster.nosql.status == NST_STATUS_ON) {

        global.nuster.nosql.memory = nst_memory_create("nosql.shm",
                global.nuster.nosql.dict_size + global.nuster.nosql.data_size,
                global.tune.bufsize, NST_DEFAULT_CHUNK_SIZE);

        if(!global.nuster.nosql.memory) {
            goto shm_err;
        }

        if(nst_shctx_init(global.nuster.nosql.memory) != NST_OK) {
            goto shm_err;
        }

        nuster.nosql = nst_memory_alloc(global.nuster.nosql.memory, sizeof(nst_core_t));

        if(!nuster.nosql) {
            goto err;
        }

        memset(nuster.nosql, 0, sizeof(*nuster.nosql));

        nuster.nosql->memory = global.nuster.nosql.memory;
        nuster.nosql->root   = global.nuster.nosql.root;

        if(nst_store_init(global.nuster.nosql.root, &nuster.nosql->store,
                    global.nuster.nosql.memory) != NST_OK) {

            goto err;
        }

        if(nst_dict_init(&nuster.nosql->dict, &nuster.nosql->store, global.nuster.nosql.memory,
                    global.nuster.nosql.dict_size) != NST_OK) {

            goto err;
        }

        ha_notice("[nuster][nosql] on, dict_size=%"PRIu64", data_size=%"PRIu64"\n",
                global.nuster.nosql.dict_size, global.nuster.nosql.data_size);
    }

    return;

err:
    ha_alert("Out of memory when initializing nuster nosql.\n");
    exit(1);

shm_err:
    ha_alert("Error when initializing nosql.\n");
    exit(1);
}

/*
 * return 1 if the request is done, otherwise 0
 */
int
nst_nosql_check_applet(hpx_stream_t *s, hpx_channel_t *req, hpx_proxy_t *px) {

    if(global.nuster.nosql.status == NST_STATUS_ON && px->nuster.mode == NST_MODE_NOSQL) {
        hpx_stream_interface_t  *si     = &s->si[1];
        hpx_appctx_t            *appctx = NULL;

        if(s->txn->meth != HTTP_METH_GET &&
                s->txn->meth != HTTP_METH_POST &&
                s->txn->meth != HTTP_METH_DELETE) {

            nst_http_reply(s, NST_HTTP_405);

            return 1;
        }

        s->target = &nuster.applet.nosql.obj_type;

        if(unlikely(!si_register_handler(si, objt_applet(s->target)))) {
            nst_http_reply(s, NST_HTTP_500);

            if(!(s->flags & SF_ERR_MASK)) {
                s->flags |= SF_ERR_LOCAL;
            }

            return 1;
        } else {
            appctx      = si_appctx(si);
            appctx->st0 = NST_NOSQL_APPCTX_STATE_INIT;
            appctx->st1 = 0;
            appctx->st2 = 0;

            req->analysers &= (AN_REQ_HTTP_BODY | AN_REQ_FLT_HTTP_HDRS | AN_REQ_FLT_END);
            req->analysers &= ~AN_REQ_FLT_XFER_DATA;
            req->analysers |= AN_REQ_HTTP_XFER_BODY;

        }
    }

    return 0;
}

nst_ring_item_t *
_nst_nosql_create_header(hpx_stream_t *s, nst_http_txn_t *txn) {
    nst_ring_item_t     *item_sl, *item_ct, *item_te, *item_eoh, *tail;
    hpx_htx_blk_type_t   type;
    hpx_htx_sl_t        *sl;
    uint32_t             size, info;
    hpx_ist_t            ctk  = ist("content-type");
    hpx_ist_t            tek  = ist("transfer-encoding");
    hpx_ist_t            tev  = ist("chunked");
    hpx_ist_t            p1   = ist("HTTP/1.1");
    hpx_ist_t            p2   = ist("200");
    hpx_ist_t            p3   = ist("OK");
    char                *data = NULL;

    item_sl = item_ct = item_te = item_eoh = tail = NULL;

    /* status line */
    type  = HTX_BLK_RES_SL;
    info  = type << 28;
    size  = sizeof(*sl) + p1.len + p2.len + p3.len;
    info += size;

    txn->res.header_len += 4 + size;

    item_sl = nst_ring_alloc_item(&nuster.nosql->store.ring, size);

    if(!item_sl) {
        goto err;
    }

    data = item_sl->data;

    sl = (hpx_htx_sl_t *)data;
    sl->hdrs_bytes = -1;

    sl->flags = HTX_SL_F_IS_RESP|HTX_SL_F_VER_11|HTX_SL_F_XFER_ENC|HTX_SL_F_XFER_LEN|HTX_SL_F_CHNK;

    HTX_SL_P1_LEN(sl) = p1.len;
    HTX_SL_P2_LEN(sl) = p2.len;
    HTX_SL_P3_LEN(sl) = p3.len;
    memcpy(HTX_SL_P1_PTR(sl), p1.ptr, p1.len);
    memcpy(HTX_SL_P2_PTR(sl), p2.ptr, p2.len);
    memcpy(HTX_SL_P3_PTR(sl), p3.ptr, p3.len);

    item_sl->info = info;

    tail = item_sl;

    /* content-type */
    type  = HTX_BLK_HDR;
    info  = type << 28;
    size  = ctk.len + txn->req.content_type.len;
    info += (txn->req.content_type.len << 8) + ctk.len;

    item_ct = nst_ring_alloc_item(&nuster.nosql->store.ring, size);

    if(!item_ct) {
        goto err;
    }

    data = item_ct->data;

    txn->res.header_len += 4 + size;

    ist2bin_lc(data, ctk);
    memcpy(data + ctk.len, txn->req.content_type.ptr, txn->req.content_type.len);

    item_ct->info = info;

    tail->next = item_ct;
    tail       = item_ct;

    /* transfer-encoding */
    type  = HTX_BLK_HDR;
    info  = type << 28;
    size  = tek.len + tev.len;
    info += (tev.len << 8) + tek.len;

    item_te = nst_ring_alloc_item(&nuster.nosql->store.ring, size);

    if(!item_te) {
        goto err;
    }

    data = item_te->data;

    txn->res.header_len += 4 + size;

    ist2bin_lc(data, tek);
    memcpy(data + tek.len, tev.ptr, tev.len);

    item_te->info = info;

    tail->next = item_te;
    tail       = item_te;

    /* eoh */
    type  = HTX_BLK_EOH;
    info  = type << 28;
    size  = 1;
    info += size;

    item_eoh = nst_ring_alloc_item(&nuster.nosql->store.ring, size);

    if(!item_eoh) {
        goto err;
    }

    data = item_eoh->data;

    txn->res.header_len += 4 + size;

    item_eoh->info = info;

    tail->next = item_eoh;
    tail       = item_eoh;
    tail->next = NULL;

    return item_sl;

err:
    nst_memory_free(nuster.nosql->memory, item_sl);
    nst_memory_free(nuster.nosql->memory, item_ct);
    nst_memory_free(nuster.nosql->memory, item_te);
    nst_memory_free(nuster.nosql->memory, item_eoh);

    return NULL;
}

void
nst_nosql_create(hpx_stream_t *s, hpx_http_msg_t *msg, nst_ctx_t *ctx) {
    nst_dict_entry_t    *entry  = NULL;
    nst_ring_item_t     *item   = NULL;
    nst_ring_item_t     *header = NULL;
    nst_key_t           *key;
    int                  idx;

    header = _nst_nosql_create_header(s, &ctx->txn);

    if(header == NULL) {
        ctx->state = NST_CTX_STATE_FULL;

        return;
    }

    idx = ctx->rule->key->idx;
    key = &(ctx->keys[idx]);

    ctx->state = NST_CTX_STATE_CREATE;

    nst_shctx_lock(&nuster.nosql->dict);

    entry = nst_dict_get(&nuster.nosql->dict, key);

    if(entry) {

        if(entry->state == NST_DICT_ENTRY_STATE_VALID) {
            entry->state = NST_DICT_ENTRY_STATE_UPDATE;
        }

        ctx->state   = NST_CTX_STATE_UPDATE;
        ctx->entry   = entry;
    }

    if(ctx->state == NST_CTX_STATE_CREATE) {
        entry = nst_dict_set(&nuster.nosql->dict, key, &ctx->txn, ctx->rule, ctx->pid);

        if(entry) {
            ctx->state = NST_CTX_STATE_CREATE;
            ctx->entry = entry;
        } else {
            ctx->state = NST_CTX_STATE_FULL;
        }
    }

    nst_shctx_unlock(&nuster.nosql->dict);

    /* init store data */

    if(ctx->state == NST_CTX_STATE_CREATE || ctx->state == NST_CTX_STATE_UPDATE) {
        if(nst_store_memory_on(ctx->rule->store)) {
            ctx->store.ring.data = nst_ring_store_init(&nuster.nosql->store.ring);
        }

        if(nst_store_disk_on(ctx->rule->store)) {
            uint64_t  t = ctx->rule->ttl;

            t = t << 32;

            *( uint8_t *)(&t)      = ctx->rule->extend[0];
            *((uint8_t *)(&t) + 1) = ctx->rule->extend[1];
            *((uint8_t *)(&t) + 2) = ctx->rule->extend[2];
            *((uint8_t *)(&t) + 3) = ctx->rule->extend[3];

            nst_disk_store_init(&nuster.nosql->store.disk, &ctx->store.disk, key, &ctx->txn, t);
        }
    }

    /* create header */

    if(ctx->state == NST_CTX_STATE_CREATE || ctx->state == NST_CTX_STATE_UPDATE) {

        if(nst_store_memory_on(ctx->rule->store) && ctx->store.ring.data) {
            ctx->store.ring.data->item = header;

            item = header;

            while(item) {
                ctx->store.ring.item = item;

                item = item->next;
            }
        }

        if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {
            item = header;

            while(item) {
                int  sz = ((item->info & 0xff) + ((item->info >> 8) & 0xfffff));

                nst_disk_store_add(&nuster.nosql->store.disk, &ctx->store.disk,
                        (char *)&item->info, 4);

                nst_disk_store_add(&nuster.nosql->store.disk, &ctx->store.disk, item->data, sz);

                item = item->next;
            }
        }

        if(nst_store_memory_off(ctx->rule->store)) {
            item = header;

            while(item) {
                nst_ring_item_t  *t = item;

                item = item->next;

                nst_memory_free(nuster.nosql->memory, t);
            }
        }
    }

err:
    return;
}

int
nst_nosql_update(hpx_http_msg_t *msg, nst_ctx_t *ctx, unsigned int offset, unsigned int len) {
    hpx_htx_blk_type_t  type;
    hpx_htx_ret_t       htxret;
    hpx_htx_blk_t      *blk;
    hpx_htx_t          *htx;
    unsigned int        forward = 0;

    htx    = htxbuf(&msg->chn->buf);
    htxret = htx_find_offset(htx, offset);
    blk    = htxret.blk;
    offset = htxret.ret;

    for(; blk && len; blk = htx_get_next_blk(htx, blk)) {
        hpx_ist_t  data;
        uint32_t   info;

        type = htx_get_blk_type(blk);

        if(type == HTX_BLK_DATA) {
            data = htx_get_blk_value(htx, blk);
            data.ptr += offset;
            data.len -= offset;

            if(data.len > len) {
                data.len = len;
            }

            info = (type << 28) + data.len;

            ctx->txn.res.payload_len += data.len;

            forward += data.len;
            len     -= data.len;

            if(nst_store_memory_on(ctx->rule->store) && ctx->store.ring.data) {
                int  ret;

                ret = nst_ring_store_add(&nuster.nosql->store.ring, ctx->store.ring.data,
                        &ctx->store.ring.item, data.ptr, data.len, info);

                if(ret == NST_ERR) {
                    ctx->store.ring.data = NULL;
                }
            }

            if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {
                nst_disk_store_add(&nuster.nosql->store.disk, &ctx->store.disk, data.ptr, data.len);
            }
        }

        if(type == HTX_BLK_TLR || type == HTX_BLK_EOT) {
            uint32_t  sz = htx_get_blksz(blk);

            forward += sz;
            len     -= sz;

            if(nst_store_memory_on(ctx->rule->store) && ctx->store.ring.data) {
                int  ret;

                ret = nst_ring_store_add(&nuster.nosql->store.ring, ctx->store.ring.data,
                        &ctx->store.ring.item, htx_get_blk_ptr(htx, blk), sz, blk->info);

                if(ret == NST_ERR) {
                    ctx->store.ring.data = NULL;
                }
            }

            if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {
                nst_disk_store_add(&nuster.nosql->store.disk, &ctx->store.disk,
                        (char *)&blk->info, 4);

                nst_disk_store_add(&nuster.nosql->store.disk, &ctx->store.disk,
                        htx_get_blk_ptr(htx, blk), sz);
            }
        }

        if(type == HTX_BLK_EOM) {
            uint32_t  sz = htx_get_blksz(blk);

            forward += sz;
            len     -= sz;
        }

        offset = 0;
    }

    return forward;
}

void
nst_nosql_finish(hpx_stream_t *s, hpx_http_msg_t *msg, nst_ctx_t *ctx) {
    hpx_htx_blk_type_t  type;
    uint32_t            size, info;
    nst_key_t          *key;
    int                 idx;

    type  = HTX_BLK_EOT;
    info  = type << 28;
    size  = 1;
    info += size;

    idx = ctx->rule->key->idx;
    key = &(ctx->keys[idx]);

    ctx->state = NST_CTX_STATE_DONE;

    ctx->entry->ctime = get_current_timestamp();

    if(ctx->rule->ttl == 0) {
        ctx->entry->expire = 0;
    } else {
        ctx->entry->expire = get_current_timestamp() / 1000 + ctx->rule->ttl;
    }

    if(nst_store_memory_on(ctx->rule->store) && ctx->store.ring.data) {

        if(!(msg->flags & HTTP_MSGF_TE_CHNK)) {
            int  ret;

            ret = nst_ring_store_add(&nuster.nosql->store.ring, ctx->store.ring.data,
                    &ctx->store.ring.item, "", size, info);

            if(ret == NST_ERR) {
                ctx->store.ring.data = NULL;
            }
        }
    }

    if(nst_store_memory_on(ctx->rule->store) && ctx->store.ring.data) {

        nst_shctx_lock(&nuster.nosql->dict);

        if(ctx->entry && ctx->entry->state != NST_DICT_ENTRY_STATE_INVALID
                && ctx->entry->store.ring.data) {

            ctx->entry->store.ring.data->invalid = 1;

            nst_ring_incr_invalid(&nuster.nosql->store.ring);
        }

        ctx->entry->state = NST_DICT_ENTRY_STATE_VALID;
        ctx->entry->store.ring.data = ctx->store.ring.data;

        nst_shctx_unlock(&nuster.nosql->dict);
    }

    if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {

        if(!(msg->flags & HTTP_MSGF_TE_CHNK)) {
            nst_disk_store_add(&nuster.nosql->store.disk, &ctx->store.disk, (char *)&info, 4);
            nst_disk_store_add(&nuster.nosql->store.disk, &ctx->store.disk, "", size);
        }
    }

    if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {

        if(nst_disk_store_end(&nuster.nosql->store.disk, &ctx->store.disk, key, &ctx->txn,
                    ctx->entry->expire) == NST_OK) {

            ctx->entry->state = NST_DICT_ENTRY_STATE_VALID;

            ctx->entry->store.disk.file = ctx->store.disk.file;
        }
    }


    if(ctx->entry->state != NST_DICT_ENTRY_STATE_VALID) {
        ctx->state = NST_CTX_STATE_INVALID;
        ctx->entry->state = NST_DICT_ENTRY_STATE_INIT;
    }

}

int
nst_nosql_exists(nst_ctx_t *ctx) {
    nst_dict_entry_t  *entry = NULL;
    nst_key_t         *key;
    int                ret, idx;

    ret = NST_CTX_STATE_INIT;
    idx = ctx->rule->key->idx;
    key = &(ctx->keys[idx]);

    if(!key) {
        return ret;
    }

    if(!nst_key_memory_checked(key)) {
        nst_key_memory_set_checked(key);

        nst_shctx_lock(&nuster.nosql->dict);

        entry = nst_dict_get(&nuster.nosql->dict, key);

        if(entry) {

            if(entry->state == NST_DICT_ENTRY_STATE_VALID
                    || entry->state == NST_DICT_ENTRY_STATE_UPDATE) {

                if(entry->store.ring.data) {
                    ctx->store.ring.data = entry->store.ring.data;
                    nst_ring_data_attach(&nuster.nosql->store.ring, ctx->store.ring.data);
                    ret = NST_CTX_STATE_HIT_MEMORY;
                } else if(entry->store.disk.file) {
                    ctx->store.disk.file = entry->store.disk.file;
                    ret = NST_CTX_STATE_HIT_DISK;
                }
            }

            if(entry->state == NST_DICT_ENTRY_STATE_INIT) {
                ret = NST_CTX_STATE_INIT;
            }
        }

        nst_shctx_unlock(&nuster.nosql->dict);
    }

    if(ret == NST_CTX_STATE_INIT) {

        if(!nst_store_disk_off(ctx->rule->store)) {

            if(!nuster.nosql->store.disk.loaded) {
                ret = NST_CTX_STATE_CHECK_DISK;
            }
        }
    }

    if(ret == NST_CTX_STATE_HIT_MEMORY) {
        return ret;
    }

    if(ret == NST_CTX_STATE_HIT_DISK) {

        if(!nst_key_disk_checked(key)) {
            nst_key_disk_set_checked(key);

            if(ctx->store.disk.file) {
                if(nst_disk_data_valid(&ctx->store.disk, key) != NST_OK) {
                    ret = NST_CTX_STATE_INIT;

                    if(entry && entry->state == NST_DICT_ENTRY_STATE_VALID) {
                        entry->state = NST_DICT_ENTRY_STATE_INVALID;
                    }
                }
            } else {
                ret = NST_CTX_STATE_INIT;
            }
        }
    }

    if(ret == NST_CTX_STATE_CHECK_DISK) {

        if(!nst_key_disk_checked(key)) {
            nst_key_disk_set_checked(key);

            if(nst_disk_data_exists(&nuster.nosql->store.disk, &ctx->store.disk, key) == NST_OK) {
                ret = NST_CTX_STATE_HIT_DISK;
            } else {
                ret = NST_CTX_STATE_INIT;
            }
        } else {
            ret = NST_CTX_STATE_INIT;
        }
    }

    return ret;
}

void
nst_nosql_abort(nst_ctx_t *ctx) {

    if(ctx->entry->state == NST_DICT_ENTRY_STATE_INIT
            || ctx->entry->state == NST_DICT_ENTRY_STATE_UPDATE) {

        if(ctx->store.ring.data) {
            nst_ring_store_abort(&nuster.nosql->store.ring, ctx->store.ring.data);
        }

        if(ctx->store.disk.file) {
            nst_disk_store_abort(&nuster.nosql->store.disk, &ctx->store.disk);
        }
    }

    ctx->entry->state = NST_DICT_ENTRY_STATE_INVALID;
}

/*
 * -1: error
 *  0: not found
 *  1: ok
 */
int
nst_nosql_delete(nst_key_t *key) {
    nst_dict_entry_t  *entry = NULL;
    int                ret   = 0;

    nst_shctx_lock(&nuster.nosql->dict);

    entry = nst_dict_get(&nuster.nosql->dict, key);

    if(entry) {

        if(entry->state == NST_DICT_ENTRY_STATE_VALID
                || entry->state == NST_DICT_ENTRY_STATE_UPDATE) {

            entry->state  = NST_DICT_ENTRY_STATE_INIT;
            entry->expire = 0;

            if(entry->store.ring.data) {
                entry->store.ring.data->invalid = 1;
                entry->store.ring.data          = NULL;

                nst_ring_incr_invalid(&nuster.nosql->store.ring);
            }

            if(entry->store.disk.file) {
                nst_disk_remove(entry->store.disk.file);
                nst_memory_free(nuster.nosql->memory, entry->store.disk.file);
                entry->store.disk.file = NULL;
            }

            ret = 1;
        }

    } else {
        ret = 0;
    }

    nst_shctx_unlock(&nuster.nosql->dict);

    if(!nuster.nosql->store.disk.loaded && global.nuster.nosql.root.len){
        nst_disk_data_t  disk;
        hpx_buffer_t    *buf = get_trash_chunk();

        disk.file = buf->area;

        ret = nst_disk_purge_by_key(global.nuster.nosql.root, &disk, key);
    }

    return ret;
}

