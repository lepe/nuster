/*
 * nuster cache engine functions.
 *
 * Copyright (C) Jiang Wenyuan, < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <proto/log.h>
#include <proto/http_ana.h>
#include <proto/raw_sock.h>
#include <proto/stream_interface.h>
#include <proto/acl.h>
#include <proto/proxy.h>
#include <proto/http_htx.h>
#include <common/htx.h>

#ifdef USE_OPENSSL
#include <proto/ssl_sock.h>
#include <types/ssl_sock.h>
#endif

#include <nuster/nuster.h>

static void
_nst_cache_memory_handler(hpx_appctx_t *appctx) {
    hpx_stream_interface_t  *si  = appctx->owner;
    hpx_channel_t           *req = si_oc(si);
    hpx_channel_t           *res = si_ic(si);
    hpx_htx_t               *req_htx, *res_htx;
    nst_ring_item_t         *item = NULL;
    int                      total = 0;

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

    if(res->flags & (CF_SHUTW|CF_SHUTR|CF_SHUTW_NOW)) {
        appctx->ctx.nuster.store.ring.item = NULL;
    }

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
    }

out:
    appctx->ctx.nuster.store.ring.item = item;
    total = res_htx->data - total;

    if(total) {
        channel_add_input(res, total);
    }

    htx_to_buf(res_htx, &res->buf);
}

/*
 * The cache disk applet acts like the backend to send cached http data
 */
static void
_nst_cache_disk_handler(hpx_appctx_t *appctx) {
    hpx_stream_interface_t  *si = appctx->owner;
    hpx_channel_t           *req = si_oc(si);
    hpx_channel_t           *res = si_ic(si);
    hpx_buffer_t            *buf;
    hpx_htx_t               *req_htx, *res_htx;
    hpx_htx_blk_type_t       type;
    hpx_htx_blk_t           *blk;
    char                    *p, *ptr;
    uint64_t                 offset, payload_len;
    uint32_t                 blksz, sz, info;
    int                      total, ret, max, fd, header_len;


    header_len  = appctx->ctx.nuster.store.disk.header_len;
    payload_len = appctx->ctx.nuster.store.disk.payload_len;
    offset      = appctx->ctx.nuster.store.disk.offset;
    fd          = appctx->ctx.nuster.store.disk.fd;

    res_htx = htxbuf(&res->buf);
    total   = res_htx->data;

    if(unlikely(si->state == SI_ST_DIS || si->state == SI_ST_CLO)) {
        goto out;
    }

    /* Check if the input buffer is avalaible. */
    if(res->buf.size == 0) {
        si_rx_room_blk(si);

        goto out;
    }

    /* check that the output is not closed */
    if(res->flags & (CF_SHUTW|CF_SHUTR|CF_SHUTW_NOW)) {
        appctx->st1 = NST_DISK_APPLET_DONE;
    }

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

            appctx->ctx.nuster.store.disk.offset = offset;

        case NST_DISK_APPLET_PAYLOAD:
            buf = get_trash_chunk();
            p   = buf->area;
            max = htx_get_max_blksz(res_htx, channel_htx_recv_max(res, res_htx));

            if(max <= 0) {
                goto out;
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
            buf = get_trash_chunk();
            p   = buf->area;
            max = htx_get_max_blksz(res_htx, channel_htx_recv_max(res, res_htx));

            ret = pread(fd, p, max, offset);

            if(ret < 0) {
                appctx->st1 = NST_DISK_APPLET_ERROR;

                break;
            }

            if(ret > 0) {
                max = ret;

                while(max != 0) {
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

                    max -= 4 + sz;
                }

                offset += ret;

                appctx->ctx.nuster.store.disk.offset = offset;

                break;
            }

            appctx->st1 = NST_DISK_APPLET_END;

            close(fd);

        case NST_DISK_APPLET_END:
            if(!htx_add_endof(res_htx, HTX_BLK_EOM)) {
                si_rx_room_blk(si);

                goto out;
            }

            appctx->st1 = NST_DISK_APPLET_DONE;
        case NST_DISK_APPLET_DONE:

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

            return;
    }

out:
    total = res_htx->data - total;

    if(total) {
        channel_add_input(res, total);
    }

    htx_to_buf(res_htx, &res->buf);
}

/*
 * The cache applet acts like the backend to send cached http data
 */
static void
nst_cache_handler(hpx_appctx_t *appctx) {

    if(appctx->st0 == NST_CTX_STATE_HIT_MEMORY) {
        _nst_cache_memory_handler(appctx);
    } else {
        _nst_cache_disk_handler(appctx);
    }
}

void
nst_cache_housekeeping() {
    uint64_t  start;

    if(global.nuster.cache.status == NST_STATUS_ON && master == 1) {
        int  dict_cleaner = global.nuster.cache.dict_cleaner;
        int  data_cleaner = global.nuster.cache.data_cleaner;
        int  disk_cleaner = global.nuster.cache.disk_cleaner;
        int  disk_loader  = global.nuster.cache.disk_loader;
        int  disk_saver   = global.nuster.cache.disk_saver;
        int  ms           = 10;
        int  ratio        = 1;

        start = get_current_timestamp();

        while(dict_cleaner--) {
            nst_dict_cleanup(&nuster.cache->dict);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

        start = get_current_timestamp();

        if(data_cleaner > nuster.cache->store.ring.count) {
            data_cleaner = nuster.cache->store.ring.count;
        }

        if(nuster.cache->store.ring.count) {
            ratio = nuster.cache->store.ring.invalid * 10 / nuster.cache->store.ring.count;
        }

        if(ratio >= 2) {
            data_cleaner = nuster.cache->store.ring.count;

            ms = ms * ratio ;
            ms = ms >= 100 ? 100 : ms;
        }

        while(data_cleaner--) {
            nst_ring_cleanup(&nuster.cache->store.ring);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

        start = get_current_timestamp();

        while(disk_cleaner--) {
            nst_disk_cleanup(nuster.cache);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

        start = get_current_timestamp();

        while(disk_loader--) {
            nst_disk_load(nuster.cache);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

        start = get_current_timestamp();

        while(disk_saver--) {
            nst_ring_store_sync(nuster.cache);

            if(get_current_timestamp() - start >= ms) {
                break;
            }
        }

    }
}

void
nst_cache_init() {
    nuster.applet.cache.fct = nst_cache_handler;

    if(global.nuster.cache.status == NST_STATUS_ON) {

        global.nuster.cache.memory = nst_memory_create("cache.shm",
                global.nuster.cache.dict_size + global.nuster.cache.data_size,
                global.tune.bufsize, NST_DEFAULT_CHUNK_SIZE);

        if(!global.nuster.cache.memory) {
            goto shm_err;
        }

        if(nst_shctx_init(global.nuster.cache.memory) != NST_OK) {
            goto shm_err;
        }

        nuster.cache = nst_memory_alloc(global.nuster.cache.memory, sizeof(nst_core_t));

        if(!nuster.cache) {
            goto err;
        }

        memset(nuster.cache, 0, sizeof(*nuster.cache));

        nuster.cache->memory = global.nuster.cache.memory;
        nuster.cache->root   = global.nuster.cache.root;

        if(nst_store_init(global.nuster.cache.root, &nuster.cache->store,
                    global.nuster.cache.memory) != NST_OK) {

            goto err;
        }

        if(nst_dict_init(&nuster.cache->dict, &nuster.cache->store, global.nuster.cache.memory,
                    global.nuster.cache.dict_size) != NST_OK) {

            goto err;
        }

        ha_notice("[nuster][cache] on, dict_size=%"PRIu64", data_size=%"PRIu64"\n",
                global.nuster.cache.dict_size, global.nuster.cache.data_size);
    }

    return;

err:
    ha_alert("Out of memory when initializing cache.\n");
    exit(1);

shm_err:
    ha_alert("Error when initializing cache.\n");
    exit(1);
}

static void
_nst_cache_record_access(nst_dict_entry_t *entry) {

    if(entry->expire == 0 || entry->extend[0] == 0xFF) {
        entry->access[0]++;
    } else {
        uint64_t  stime, diff;
        float     pct;
        uint32_t  ttl = entry->ttl;

        stime = entry->ctime + ttl * entry->extended * 1000;
        diff  = entry->atime - stime;
        pct   = diff / 1000.0 / ttl * 100;

        if(pct < 100 - entry->extend[0] - entry->extend[1] - entry->extend[2]) {
            entry->access[0]++;
        } else if(pct < 100 - entry->extend[1] - entry->extend[2]) {
            entry->access[1]++;
        } else if(pct < 100 - entry->extend[2]) {
            entry->access[2]++;
        } else {
            entry->access[3]++;
        }
    }

}

void
nst_cache_create(hpx_http_msg_t *msg, nst_ctx_t *ctx) {
    hpx_htx_blk_type_t  type;
    hpx_htx_blk_t      *blk;
    hpx_htx_t          *htx;
    nst_key_t          *key;
    nst_dict_entry_t   *entry;
    uint32_t            sz;
    int                 idx;

    idx = ctx->rule->key->idx;
    key = &(ctx->keys[idx]);

    htx = htxbuf(&msg->chn->buf);

    ctx->state = NST_CTX_STATE_CREATE;

    nst_shctx_lock(&nuster.cache->dict);

    entry = nst_dict_get(&nuster.cache->dict, key);

    if(entry) {
        ctx->state = NST_CTX_STATE_BYPASS;
    }

    if(ctx->state == NST_CTX_STATE_CREATE) {
        entry = nst_dict_set(&nuster.cache->dict, key, &ctx->txn, ctx->rule, ctx->pid);

        if(entry) {
            ctx->state = NST_CTX_STATE_CREATE;
            ctx->entry = entry;
        } else {
            ctx->state = NST_CTX_STATE_BYPASS;
        }
    }

    nst_shctx_unlock(&nuster.cache->dict);

    /* init store data */

    if(ctx->state == NST_CTX_STATE_CREATE) {

        if(nst_store_memory_on(ctx->rule->store)) {
            ctx->store.ring.data = nst_ring_store_init(&nuster.cache->store.ring);
        }

        if(nst_store_disk_on(ctx->rule->store)) {
            uint64_t  t = ctx->rule->ttl;

            t = t << 32;

            *( uint8_t *)(&t)      = ctx->rule->extend[0];
            *((uint8_t *)(&t) + 1) = ctx->rule->extend[1];
            *((uint8_t *)(&t) + 2) = ctx->rule->extend[2];
            *((uint8_t *)(&t) + 3) = ctx->rule->extend[3];

            nst_disk_store_init(&nuster.cache->store.disk, &ctx->store.disk, key, &ctx->txn, t);
        }
    }

    /* add header */

    if(ctx->state == NST_CTX_STATE_CREATE) {

        ctx->txn.res.header_len  = 0;
        ctx->txn.res.payload_len = 0;

        for(idx = htx_get_first(htx); idx != -1; idx = htx_get_next(htx, idx)) {
            blk  = htx_get_blk(htx, idx);
            sz   = htx_get_blksz(blk);
            type = htx_get_blk_type(blk);

            if(type == HTX_BLK_UNUSED) {
                continue;
            }

            ctx->txn.res.header_len += 4 + sz;

            if(nst_store_memory_on(ctx->rule->store) && ctx->store.ring.data) {
                int  ret;

                ret = nst_ring_store_add(&nuster.cache->store.ring, ctx->store.ring.data,
                        &ctx->store.ring.item, htx_get_blk_ptr(htx, blk), sz, blk->info);

                if(ret == NST_ERR) {
                    ctx->store.ring.data = NULL;
                }
            }

            if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {
                nst_disk_store_add(&nuster.cache->store.disk, &ctx->store.disk,
                        (char *)&blk->info, 4);

                nst_disk_store_add(&nuster.cache->store.disk, &ctx->store.disk,
                        htx_get_blk_ptr(htx, blk), sz);
            }

            if(type == HTX_BLK_EOH) {
                break;
            }
        }
    }

err:
    return;
}

/*
 * Add partial http data to nst_ring_data
 */
unsigned int
nst_cache_update(hpx_http_msg_t *msg, nst_ctx_t *ctx, unsigned int offset, unsigned int len) {
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

                ret = nst_ring_store_add(&nuster.cache->store.ring, ctx->store.ring.data,
                        &ctx->store.ring.item, data.ptr, data.len, info);

                if(ret == NST_ERR) {
                    ctx->store.ring.data = NULL;
                }
            }

            if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {
                nst_disk_store_add(&nuster.cache->store.disk, &ctx->store.disk, data.ptr, data.len);
            }
        }

        if(type == HTX_BLK_TLR || type == HTX_BLK_EOT) {
            uint32_t  sz = htx_get_blksz(blk);

            forward += sz;
            len     -= sz;

            if(nst_store_memory_on(ctx->rule->store) && ctx->store.ring.data) {
                int  ret;

                ret = nst_ring_store_add(&nuster.cache->store.ring, ctx->store.ring.data,
                        &ctx->store.ring.item, htx_get_blk_ptr(htx, blk), sz, blk->info);

                if(ret == NST_ERR) {
                    ctx->store.ring.data = NULL;
                }
            }

            if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {
                nst_disk_store_add(&nuster.cache->store.disk, &ctx->store.disk,
                        (char *)&blk->info, 4);

                nst_disk_store_add(&nuster.cache->store.disk, &ctx->store.disk,
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

/*
 * cache done
 */
int
nst_cache_finish(nst_ctx_t *ctx) {
    nst_key_t  *key;
    int         idx;

    idx = ctx->rule->key->idx;
    key = &(ctx->keys[idx]);

    ctx->state = NST_CTX_STATE_DONE;

    ctx->entry->ctime = get_current_timestamp();

    if(ctx->rule->ttl == 0) {
        ctx->entry->expire = 0;
    } else {
        ctx->entry->expire = ctx->entry->ctime / 1000 + ctx->rule->ttl;
    }

    ctx->entry->header_len  = ctx->txn.res.header_len;
    ctx->entry->payload_len = ctx->txn.res.payload_len;

    if(nst_store_memory_on(ctx->rule->store) && ctx->store.ring.data) {
        ctx->entry->state = NST_DICT_ENTRY_STATE_VALID;

        ctx->entry->store.ring.data = ctx->store.ring.data;
    }

    if(nst_store_disk_on(ctx->rule->store) && ctx->store.disk.file) {

        if(nst_disk_store_end(&nuster.cache->store.disk, &ctx->store.disk, key, &ctx->txn,
                    ctx->entry->expire) == NST_OK) {

            ctx->entry->state = NST_DICT_ENTRY_STATE_VALID;

            ctx->entry->store.disk.file = ctx->store.disk.file;
        }
    }

    if(ctx->entry->state == NST_DICT_ENTRY_STATE_INIT) {
        ctx->entry->state = NST_DICT_ENTRY_STATE_INVALID;

        return NST_ERR;
    }

    return NST_OK;
}

/*
 * Check if valid cache exists
 */
int
nst_cache_exists(nst_ctx_t *ctx) {
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

        nst_shctx_lock(&nuster.cache->dict);

        entry = nst_dict_get(&nuster.cache->dict, key);

        if(entry) {

            if(entry->state == NST_DICT_ENTRY_STATE_VALID) {

                if(entry->store.ring.data) {
                    ctx->store.ring.data = entry->store.ring.data;
                    ret = NST_CTX_STATE_HIT_MEMORY;
                } else if(entry->store.disk.file) {
                    ctx->store.disk.file = entry->store.disk.file;
                    ret = NST_CTX_STATE_HIT_DISK;
                }

                ctx->txn.res.header_len    = entry->header_len;
                ctx->txn.res.payload_len   = entry->payload_len;
                ctx->txn.res.etag          = entry->etag;
                ctx->txn.res.last_modified = entry->last_modified;

                _nst_cache_record_access(entry);
            }

            if(entry->state == NST_DICT_ENTRY_STATE_INIT) {
                ctx->rule = entry->rule;
                ret = NST_CTX_STATE_WAIT;
            }
        }

        nst_shctx_unlock(&nuster.cache->dict);
    }

    if(ret == NST_CTX_STATE_INIT) {

        if(!nst_store_disk_off(ctx->rule->store)) {

            if(!nuster.cache->store.disk.loaded) {
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

            if(nst_disk_data_exists(&nuster.cache->store.disk, &ctx->store.disk, key) == NST_OK) {
                ret = NST_CTX_STATE_HIT_DISK;

                ctx->txn.res.header_len  = nst_disk_meta_get_header_len(ctx->store.disk.meta);
                ctx->txn.res.payload_len = nst_disk_meta_get_payload_len(ctx->store.disk.meta);

                if(ctx->rule->etag == NST_STATUS_ON) {
                    ctx->txn.res.etag.ptr = ctx->txn.buf->area + ctx->txn.buf->data;
                    ctx->txn.res.etag.len = nst_disk_meta_get_etag_len(ctx->store.disk.meta);

                    nst_disk_read_etag(&ctx->store.disk, ctx->txn.res.etag);
                }

                if(ctx->rule->last_modified == NST_STATUS_ON) {
                    ctx->txn.res.last_modified.ptr = ctx->txn.buf->area + ctx->txn.buf->data;
                    ctx->txn.res.last_modified.len =
                        nst_disk_meta_get_last_modified_len(ctx->store.disk.meta);

                    nst_disk_read_last_modified(&ctx->store.disk, ctx->txn.res.last_modified);

                }
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
nst_cache_abort(nst_ctx_t *ctx) {

    if(ctx->entry->state == NST_DICT_ENTRY_STATE_INIT) {

        if(ctx->store.ring.data) {
            nst_ring_store_abort(&nuster.cache->store.ring, ctx->store.ring.data);
        }

        if(ctx->store.disk.file) {
            nst_disk_store_abort(&nuster.cache->store.disk, &ctx->store.disk);
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
nst_cache_delete(nst_key_t *key) {
    nst_dict_entry_t  *entry = NULL;
    int                ret   = 1;

    nst_shctx_lock(&nuster.cache->dict);

    entry = nst_dict_get(&nuster.cache->dict, key);

    if(entry) {

        if(entry->state == NST_DICT_ENTRY_STATE_VALID) {
            entry->state  = NST_DICT_ENTRY_STATE_INVALID;
            entry->expire = 0;

            if(entry->store.ring.data) {
                entry->store.ring.data->invalid = 1;
                entry->store.ring.data          = NULL;

                nst_ring_incr_invalid(&nuster.cache->store.ring);
            }

            if(entry->store.disk.file) {
                nst_disk_remove(entry->store.disk.file);
                nst_memory_free(nuster.cache->memory, entry->store.disk.file);
                entry->store.disk.file = NULL;
            }

            ret = 1;

        }
    } else {
        ret = 0;
    }

    nst_shctx_unlock(&nuster.cache->dict);

    if(!nuster.cache->store.disk.loaded && global.nuster.cache.root.len){
        nst_disk_data_t  disk;
        hpx_buffer_t    *buf = get_trash_chunk();

        disk.file = buf->area;

        ret = nst_disk_purge_by_key(global.nuster.cache.root, &disk, key);

    }

    return ret;
}

/*
 * Create cache applet to handle the request
 */
void
nst_cache_hit(hpx_stream_t *s, hpx_stream_interface_t *si, hpx_channel_t *req, hpx_channel_t *res,
        nst_ctx_t *ctx) {

    hpx_appctx_t  *appctx = NULL;

    /*
     * set backend to nuster.applet.cache
     */
    s->target = &nuster.applet.cache.obj_type;

    if(unlikely(!si_register_handler(si, objt_applet(s->target)))) {
        /* return to regular process on error */
        s->target = NULL;
    } else {
        appctx = si_appctx(si);
        memset(&appctx->ctx.nuster.store, 0, sizeof(appctx->ctx.nuster.store));

        appctx->st0 = ctx->state;

        if(ctx->state == NST_CTX_STATE_HIT_MEMORY) {
            nst_ring_data_attach(&nuster.cache->store.ring, ctx->store.ring.data);
            appctx->ctx.nuster.store.ring.data = ctx->store.ring.data;
            appctx->ctx.nuster.store.ring.item = ctx->store.ring.data->item;
        } else {
            appctx->ctx.nuster.store.disk.fd     = ctx->store.disk.fd;
            appctx->ctx.nuster.store.disk.offset = nst_disk_get_header_pos(ctx->store.disk.meta);

            appctx->ctx.nuster.store.disk.header_len =
                nst_disk_meta_get_header_len(ctx->store.disk.meta);

            appctx->ctx.nuster.store.disk.payload_len =
                nst_disk_meta_get_payload_len(ctx->store.disk.meta);
        }

        appctx->st1 = NST_DISK_APPLET_HEADER;

        req->analysers &= ~AN_REQ_FLT_HTTP_HDRS;
        req->analysers &= ~AN_REQ_FLT_XFER_DATA;

        req->analysers |= AN_REQ_FLT_END;
        req->analyse_exp = TICK_ETERNITY;

        res->flags |= CF_NEVER_WAIT;
    }
}

void
nst_cache_build_etag(hpx_stream_t *s, hpx_http_msg_t *msg, nst_ctx_t *ctx) {
    hpx_http_hdr_ctx_t  hdr = { .blk = NULL };
    hpx_htx_t          *htx;

    ctx->txn.res.etag.ptr = ctx->txn.buf->area + ctx->txn.buf->data;
    ctx->txn.res.etag.len = 0;

    htx = htxbuf(&s->res.buf);

    if(http_find_header(htx, ist("ETag"), &hdr, 1)) {
        ctx->txn.res.etag.len = hdr.value.len;

        chunk_istcat(ctx->txn.buf, hdr.value);
    } else {
        uint64_t t = get_current_timestamp();

        sprintf(ctx->txn.res.etag.ptr, "\"%08x\"", XXH32(&t, 8, 0));
        ctx->txn.res.etag.len = 10;
        b_add(ctx->txn.buf, ctx->txn.res.etag.len);

        if(ctx->rule->etag == NST_STATUS_ON) {
            http_add_header(htx, ist("Etag"), ctx->txn.res.etag);
        }
    }
}

void
nst_cache_build_last_modified(hpx_stream_t *s, hpx_http_msg_t *msg, nst_ctx_t *ctx) {
    hpx_http_hdr_ctx_t  hdr = { .blk = NULL };
    hpx_htx_t          *htx;
    int                 len = sizeof("Mon, 01 JAN 1970 00:00:00 GMT") - 1;

    htx = htxbuf(&s->res.buf);

    ctx->txn.res.last_modified.ptr = ctx->txn.buf->area + ctx->txn.buf->data;
    ctx->txn.res.last_modified.len = len;

    if(http_find_header(htx, ist("Last-Modified"), &hdr, 1)) {

        if(hdr.value.len == len) {
            chunk_istcat(ctx->txn.buf, hdr.value);
        }

    } else {
        struct tm  *tm;
        time_t      now;
        char        mon[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
            "Oct", "Nov", "Dec" };
        char        day[7][4]  = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

        time(&now);
        tm = gmtime(&now);

        sprintf(ctx->txn.res.last_modified.ptr, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                day[tm->tm_wday], tm->tm_mday, mon[tm->tm_mon],
                1900 + tm->tm_year, tm->tm_hour, tm->tm_min, tm->tm_sec);
        b_add(ctx->txn.buf, ctx->txn.res.last_modified.len);

        if(ctx->rule->last_modified == NST_STATUS_ON) {
            http_add_header(htx, ist("Last-Modified"), ctx->txn.res.last_modified);
        }
    }
}

