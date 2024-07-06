#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>
#include <pthread.h>
#include <assert.h>

#include "config.h"

#include "mpv_talloc.h"
#include "common/common.h"
#include "misc/bstr.h"
#include "misc/dispatch.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "options/options.h"
#include "aspect.h"
#include "dr_helper.h"
#include "vo.h"
#include "video/mp_image.h"
#include "sub/osd.h"
#include "osdep/atomic.h"
#include "osdep/timer.h"

#include "common/global.h"
#include "player/client.h"

#include "libmpv.h"

/*
 * mpv_render_context is managed by the host application - the host application
 * can access it any time, even if the VO is destroyed (or not created yet).
 *
 * - the libmpv user can mix render API and normal API; thus render API
 *   functions can wait on the core, but not the reverse
 * - the core does blocking calls into the VO thread, thus the VO functions
 *   can't wait on the user calling the API functions
 * - to make video timing work like it should, the VO thread waits on the
 *   render API user anyway, and the (unlikely) deadlock is avoided with
 *   a timeout
 *
 *  Locking:  mpv core > VO > mpv_render_context.lock > mp_client_api.lock
 *              > mpv_render_context.update_lock
 *  And: render thread > VO (wait for present)
 *       VO > render thread (wait for present done, via timeout)
 */

struct vo_priv {
    struct mpv_render_context *ctx; // immutable after init
};

struct mpv_render_context {
    struct mp_log *log;
    struct mpv_global *global;
    struct mp_client_api *client_api;

    // Note that an atomic was likely used here since this needs to be called before
    // render context is potentially active. And we cannot have it acquire a lock on render context
    // because client_api_lock is after render_context_lock in the locking hierarchy.
    // And having an atomic is probably easier than creating an entirely new mutex just for this.
    atomic_bool in_use;

    // --- Immutable after init
    struct mp_dispatch_queue *dispatch;
    bool advanced_control;
    struct dr_helper *dr;           // NULL if advanced_control disabled

    pthread_mutex_t control_lock;
    // --- Protected by control_lock
    mp_render_cb_control_fn control_cb;
    void *control_cb_ctx;

    pthread_mutex_t update_lock;
    pthread_cond_t update_cond;     // paired with update_lock

    // --- Protected by update_lock
    mpv_render_update_fn update_cb;
    void *update_cb_ctx;

    pthread_mutex_t lock;
    pthread_cond_t video_wait;      // paired with lock

    // --- Protected by lock
    struct vo_frame *next_frame;    // next frame to draw
    bool shutting_down;            // Set if we shouold no longer block render/flush/present 
    int64_t render_count;          // incremented when next frame can be shown

    int64_t present_count;
    int64_t expected_present_count;

    int64_t flush_count;
    int64_t expected_flush_count;

    int pending_swap_count;
    bool redrawing;                 // next_frame was a redraw request
    uint64_t last_vsync_time; // In mpv-relative units
    uint64_t vsync_interval;
    struct vo_frame *cur_frame;
    struct mp_image_params img_params;
    int vp_w, vp_h; // Note that we can't use vo->dwidth/vo->dheight directly
                    // since render is called from non-vo thread
    float vp_par;
    bool flip;
    bool imgfmt_supported[IMGFMT_END - IMGFMT_START];
    bool need_reconfig;
    bool need_resize;
    bool need_reset;
    bool need_update_external;
    struct vo *vo;


    // --- Mostly immutable after init.
    struct mp_hwdec_devices *hwdec_devs;

    // --- All of these can only be accessed from mpv_render_*() API, for
    //     which the user makes sure they're called synchronized.
    struct render_backend *renderer;
    struct m_config_cache *vo_opts_cache;
    struct mp_vo_opts *vo_opts;

    // Accessible only from VO thread
    uint64_t last_presentation_time;
};

const struct render_backend_fns *render_backends[] = {
    &render_backend_gpu,
    NULL
};

static void update(struct mpv_render_context *ctx)
{
    pthread_mutex_lock(&ctx->update_lock);
    if (ctx->update_cb)
        ctx->update_cb(ctx->update_cb_ctx);

    pthread_cond_broadcast(&ctx->update_cond);
    pthread_mutex_unlock(&ctx->update_lock);
}

void *get_mpv_render_param(mpv_render_param *params, mpv_render_param_type type,
                           void *def)
{
    for (int n = 0; params && params[n].type; n++) {
        if (params[n].type == type)
            return params[n].data;
    }
    return def;
}

static void forget_frames(struct mpv_render_context *ctx, bool all)
{
    pthread_cond_broadcast(&ctx->video_wait);
    if (all) {
        talloc_free(ctx->cur_frame);
        ctx->cur_frame = NULL;
    }
}

static void dispatch_wakeup(void *ptr)
{
    struct mpv_render_context *ctx = ptr;
    update(ctx);
}

static struct mp_image *render_get_image(void *ptr, int imgfmt, int w, int h,
                                         int stride_align)
{
    struct mpv_render_context *ctx = ptr;
    return ctx->renderer->fns->get_image(ctx->renderer, imgfmt, w, h, stride_align);
}

int mpv_render_context_create(mpv_render_context **res, mpv_handle *mpv)
{
    mpv_render_context *ctx = talloc_zero(NULL, mpv_render_context);
    pthread_mutex_init(&ctx->control_lock, NULL);
    pthread_mutex_init(&ctx->lock, NULL);

    ctx->global = mp_client_get_global(mpv);
    ctx->client_api = ctx->global->client_api;
    ctx->shutting_down = true; // start with things shut down.

    if (!mp_set_main_render_context(ctx->client_api, ctx, true)) {
        MP_ERR(ctx, "There is already a mpv_render_context set.\n");
        mpv_render_context_free(ctx);
        return MPV_ERROR_GENERIC;
    }

    *res = ctx;
    return 0;
}

int mpv_render_context_initialize(mpv_render_context *ctx, mpv_handle *mpv,
                              mpv_render_param *params)
{
    pthread_mutex_init(&ctx->update_lock, NULL);
    pthread_cond_init(&ctx->update_cond, NULL);
    pthread_cond_init(&ctx->video_wait, NULL);

    ctx->shutting_down = false;
    ctx->log = mp_log_new(ctx, ctx->global->log, "libmpv_render");
    ctx->vo_opts_cache = m_config_cache_alloc(ctx, ctx->global, &vo_sub_opts);
    ctx->vo_opts = ctx->vo_opts_cache->opts;

    assert(!ctx->dispatch);
    ctx->dispatch = mp_dispatch_create(ctx);
    mp_dispatch_set_wakeup_fn(ctx->dispatch, dispatch_wakeup, ctx);

    if (GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_ADVANCED_CONTROL, int, 0))
        ctx->advanced_control = true;

    int err = MPV_ERROR_NOT_IMPLEMENTED;
    for (int n = 0; render_backends[n]; n++) {
        ctx->renderer = talloc_zero(NULL, struct render_backend);
        *ctx->renderer = (struct render_backend){
            .global = ctx->global,
            .log = ctx->log,
            .fns = render_backends[n],
        };
        err = ctx->renderer->fns->init(ctx->renderer, params);
        if (err >= 0)
            break;
        ctx->renderer->fns->destroy(ctx->renderer);
        talloc_free(ctx->renderer->priv);
        TA_FREEP(&ctx->renderer);
        if (err != MPV_ERROR_NOT_IMPLEMENTED)
            break;
    }

    if (err < 0) {
        mpv_render_context_uninit(ctx, /*unregister=*/ true);
        mpv_render_context_free(ctx);
        return err;
    }

    ctx->hwdec_devs = ctx->renderer->hwdec_devs;

    for (int n = IMGFMT_START; n < IMGFMT_END; n++) {
        ctx->imgfmt_supported[n - IMGFMT_START] =
            ctx->renderer->fns->check_format(ctx->renderer, n);
    }

    if (ctx->renderer->fns->get_image && ctx->advanced_control)
        ctx->dr = dr_helper_create(ctx->dispatch, render_get_image, ctx);

    return 0;
}

void mpv_render_context_set_update_callback(mpv_render_context *ctx,
                                            mpv_render_update_fn callback,
                                            void *callback_ctx)
{
    pthread_mutex_lock(&ctx->update_lock);
    ctx->update_cb = callback;
    ctx->update_cb_ctx = callback_ctx;
    pthread_mutex_unlock(&ctx->update_lock);
}

void mp_render_context_set_control_callback(mpv_render_context *ctx,
                                            mp_render_cb_control_fn callback,
                                            void *callback_ctx)
{
    pthread_mutex_lock(&ctx->control_lock);
    ctx->control_cb = callback;
    ctx->control_cb_ctx = callback_ctx;
    pthread_mutex_unlock(&ctx->control_lock);
}

static void mpv_render_context_stop_flip(mpv_render_context *ctx)
{
    pthread_mutex_lock(&ctx->lock);
    ctx->shutting_down = true;
    ctx->last_vsync_time = 0;
    ctx->vsync_interval = 0;
    ctx->pending_swap_count = 0;
    pthread_mutex_unlock(&ctx->lock);
    pthread_cond_broadcast(&ctx->video_wait);
}

void mpv_render_context_uninit(mpv_render_context *ctx, bool unregister) {
    if (!ctx)
        return;


    if (unregister) {
        // From here on, ctx becomes invisible and cannot be newly acquired. Only
        // a VO could still hold a reference.
        mp_set_main_render_context(ctx->client_api, ctx, false);
    }

    if (atomic_load(&ctx->in_use)) {
        // Start destroy the VO, and also bring down the decoder etc., which
        // still might be using the hwdec context or use DR images. The above
        // mp_set_main_render_context() call guarantees it can't come back (so
        // ctx->vo can't change to non-NULL).
        // In theory, this races with vo_libmpv exiting and another VO being
        // used, which is a harmless grotesque corner case.
        // Note that if unregister=false then it is up to the API user to ensure
        // that no VO can acquire this context before this function finishes
        // (i.e. they will either use locks or dispatch queue to serialize things).
        kill_video_async(ctx->client_api);

        mpv_render_context_stop_flip(ctx);

        while (atomic_load(&ctx->in_use)) {
            // As a nasty detail, we need to wait until the VO is released, but
            // also need to react to update() calls during it (the update calls
            // are supposed to trigger processing ctx->dispatch). We solve this
            // by making the VO uninit function call mp_dispatch_interrupt().
            //
            // Other than that, processing ctx->dispatch is needed to serve the
            // video decoder, which might still not be fully destroyed, and e.g.
            // performs calls to release DR images (or, as a grotesque corner
            // case may even try to allocate new ones).
            //
            // Once the VO is released, ctx->dispatch becomes truly inactive.
            // (The libmpv API user could call mpv_render_context_update() while
            // mpv_render_context_free() is being called, but of course this is
            // invalid.)
            mp_dispatch_queue_process(ctx->dispatch, INFINITY);
        }
    }

    pthread_mutex_lock(&ctx->lock);
    // Barrier - guarantee uninit() has left the lock region. It will access ctx
    // until the lock has been released, so we must not proceed with destruction
    // before we can acquire the lock. (The opposite, uninit() acquiring the
    // lock, can not happen anymore at this point - we've waited for VO uninit,
    // and prevented that new VOs can be created.)
    pthread_mutex_unlock(&ctx->lock);

    assert(!atomic_load(&ctx->in_use));
    assert(!ctx->vo);

    // With the dispatch queue not being served anymore, allow frame free
    // requests from this thread to be served directly.
    if (ctx->dr)
        dr_helper_acquire_thread(ctx->dr);

    // Possibly remaining outstanding work.
    mp_dispatch_queue_process(ctx->dispatch, 0);

    forget_frames(ctx, true);

    if (ctx->renderer) {
        ctx->renderer->fns->destroy(ctx->renderer);
        talloc_free(ctx->renderer->priv);
        TA_FREEP(&ctx->renderer);
    }
    talloc_free(ctx->dr);
    talloc_free(ctx->dispatch);
    ctx->dispatch = NULL;

    pthread_cond_destroy(&ctx->update_cond);
    pthread_cond_destroy(&ctx->video_wait);
    pthread_mutex_destroy(&ctx->update_lock);
    TALLOC_FREE(ctx->vo_opts_cache);
    TALLOC_FREE(ctx->log);
    ctx->vo_opts = NULL;

}


void mpv_render_context_free(mpv_render_context *ctx)
{
    if (!ctx)
        return;

    assert(!atomic_load(&ctx->in_use));
    assert(!ctx->vo);
    assert(!ctx->dispatch);

    mp_set_main_render_context(ctx->client_api, ctx, false);

    pthread_mutex_destroy(&ctx->lock);
    pthread_mutex_destroy(&ctx->control_lock);

    talloc_free(ctx);
}

// Try to mark the context as "in exclusive use" (e.g. by a VO).
// Note: the function must not acquire any locks, because it's called with an
// external leaf lock held.
bool mp_render_context_acquire(mpv_render_context *ctx)
{
    bool prev = false;
    return atomic_compare_exchange_strong(&ctx->in_use, &prev, true);
}

int mpv_render_context_render(mpv_render_context *ctx, mpv_render_param *params)
{
    pthread_mutex_lock(&ctx->lock);
    assert(!ctx->shutting_down);

    int do_render =
        !GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_SKIP_RENDERING, int, 0);

    if (do_render) {
        int vp_w, vp_h;
        int err = ctx->renderer->fns->get_target_size(ctx->renderer, params,
                                                    &vp_w, &vp_h);
        if (err < 0) {
            pthread_mutex_unlock(&ctx->lock);
            return err;
        }

        if (ctx->vo && (ctx->vp_w != vp_w || ctx->vp_h != vp_h ||
                        ctx->need_resize))
        {
            m_config_cache_update(ctx->vo_opts_cache);

            ctx->vp_w = vp_w;
            ctx->vp_h = vp_h;
            ctx->vp_par = 1.0 / ctx->vo_opts->monitor_pixel_aspect;

            struct mp_rect src, dst;
            struct mp_osd_res osd;
            mp_get_src_dst_rects(ctx->log, ctx->vo_opts, ctx->vo->driver->caps,
                                &ctx->img_params, vp_w, abs(vp_h),
                                ctx->vp_par, &src, &dst, &osd);

            ctx->renderer->fns->resize(ctx->renderer, &src, &dst, &osd);
        }
        ctx->need_resize = false;
    }

    if (ctx->need_reconfig)
        ctx->renderer->fns->reconfig(ctx->renderer, &ctx->img_params);
    ctx->need_reconfig = false;

    if (ctx->need_update_external)
        ctx->renderer->fns->update_external(ctx->renderer, ctx->vo);
    ctx->need_update_external = false;

    if (ctx->need_reset) {
        ctx->renderer->fns->reset(ctx->renderer);
        if (ctx->cur_frame)
            ctx->cur_frame->still = true;
    }
    ctx->need_reset = false;

    struct vo_frame *frame = ctx->next_frame;
    int64_t wait_render_count = ctx->render_count;
    if (frame) {
        ctx->next_frame = NULL;
        if (!(frame->redraw || !frame->current))
            wait_render_count += 1;
        pthread_cond_broadcast(&ctx->video_wait);
        talloc_free(ctx->cur_frame);
        ctx->cur_frame = vo_frame_ref(frame);
    } else {
        frame = vo_frame_ref(ctx->cur_frame);
        if (frame)
            frame->redraw = true;
        MP_STATS(ctx, "glcb-noframe");
    }
    struct vo_frame dummy = {0};
    if (!frame)
        frame = &dummy;

    pthread_mutex_unlock(&ctx->lock);

    MP_STATS(ctx, "glcb-render");

    int err = 0;

    if (do_render)
        err = ctx->renderer->fns->render(ctx->renderer, params, frame);

    if (frame != &dummy)
        talloc_free(frame);

    if (GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME,
                             int, 1))
    {
        pthread_mutex_lock(&ctx->lock);
        while (wait_render_count > ctx->render_count)
            pthread_cond_wait(&ctx->video_wait, &ctx->lock);
        pthread_mutex_unlock(&ctx->lock);
    }

    return err;
}

void mpv_render_context_report_swap(mpv_render_context *ctx, uint64_t time)
{
    pthread_mutex_lock(&ctx->lock);
    if (ctx->shutting_down) {
        goto done;
    }

    ctx->pending_swap_count = time == (uint64_t) -1 ? 0 : MPMAX(ctx->pending_swap_count - 1, 0);

    uint64_t vsync_time = (time == (uint64_t) -1 ? 0 :
                         (time > 0 ? mp_time_us() - (mp_raw_time_us() - time) : mp_time_us()));


    if (ctx->last_vsync_time > 0 && vsync_time > ctx->last_vsync_time) {
        ctx->vsync_interval = ctx->vsync_interval > 0 ? (vsync_time - ctx->last_vsync_time)*0.5 + ctx->vsync_interval*0.5 : vsync_time - ctx->last_vsync_time;
    }

    if ((int64_t) vsync_time - (int64_t) ctx->last_vsync_time > 17000) {
        printf("Report swap clock skew, got %llu\n", vsync_time - ctx->last_vsync_time);
    }
    
    if (vsync_time > ctx->last_vsync_time)
        ctx->last_vsync_time = vsync_time;

    pthread_cond_broadcast(&ctx->video_wait);
done:
    pthread_mutex_unlock(&ctx->lock);

}

void mpv_render_context_wait_for_swap(mpv_render_context *ctx, bool skip)
{
    struct timespec ts = mp_rel_time_to_timespec(0.2);

    pthread_mutex_lock(&ctx->lock);
    if (ctx->shutting_down) {
        goto done;
    }
    // Wait for next vsync after flush
    // TODO: Should also use gpu fence to make sure commands itself retired.
    // See opengl vo.
    while (!skip && (ctx->pending_swap_count > 0) && !ctx->shutting_down) {
        if (pthread_cond_timedwait(&ctx->video_wait, &ctx->lock, &ts)) {
            printf("Bail on swap\n");
            MP_WARN(ctx, "mpv_render_report_swap() not being called.\n");
            goto done;
        }
    }

    ctx->present_count += 1;
    pthread_cond_broadcast(&ctx->video_wait);
done:
    pthread_mutex_unlock(&ctx->lock);
}

void mpv_render_context_report_flush(mpv_render_context *ctx)
{
    pthread_mutex_lock(&ctx->lock);
    if (ctx->shutting_down) {
        goto done;
    }
    if (ctx->expected_flush_count > ctx->flush_count) {
        ctx->pending_swap_count += 1;
    }
    ctx->flush_count += 1;
    pthread_cond_broadcast(&ctx->video_wait);
done:
    pthread_mutex_unlock(&ctx->lock);
}


uint64_t mpv_render_context_update(mpv_render_context *ctx)
{
    uint64_t res = 0;
    bool dispatch_pending = mp_dispatch_pending(ctx->dispatch);
    if (dispatch_pending)
        res |= MPV_RENDER_PROCESS_QUEUE;
    pthread_mutex_lock(&ctx->lock);
    assert(!ctx->shutting_down);
    if (ctx->next_frame)
        res |= MPV_RENDER_UPDATE_FRAME;
    pthread_mutex_unlock(&ctx->lock);
    return res;
}

void mpv_render_context_process_queue(mpv_render_context *ctx) {
    mp_dispatch_queue_process(ctx->dispatch, 0);
}

int mpv_render_context_set_parameter(mpv_render_context *ctx,
                                     mpv_render_param param)
{
    return ctx->renderer->fns->set_parameter(ctx->renderer, param);
}

int mpv_render_context_get_info(mpv_render_context *ctx,
                                mpv_render_param param)
{
    int res = MPV_ERROR_NOT_IMPLEMENTED;
    pthread_mutex_lock(&ctx->lock);

    switch (param.type) {
    case MPV_RENDER_PARAM_NEXT_FRAME_INFO: {
        mpv_render_frame_info *info = param.data;
        *info = (mpv_render_frame_info){0};
        struct vo_frame *frame = ctx->next_frame;
        if (frame) {
            info->flags =
                MPV_RENDER_FRAME_INFO_PRESENT |
                (frame->redraw ? MPV_RENDER_FRAME_INFO_REDRAW : 0) |
                (frame->repeat ? MPV_RENDER_FRAME_INFO_REPEAT : 0) |
                (frame->display_synced && !frame->redraw ?
                    MPV_RENDER_FRAME_INFO_BLOCK_VSYNC : 0);
            info->target_time = frame->pts;
        }
        res = 0;
        break;
    }
    default:;
    }

    pthread_mutex_unlock(&ctx->lock);
    return res;
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct vo_priv *p = vo->priv;
    struct mpv_render_context *ctx = p->ctx;

    pthread_mutex_lock(&ctx->lock);
    assert(!ctx->next_frame);
    ctx->next_frame = vo_frame_ref(frame);

    ctx->expected_present_count = ctx->present_count + 1;
    ctx->expected_flush_count = ctx->flush_count + 1;

    ctx->redrawing = frame->redraw || !frame->current;
    pthread_mutex_unlock(&ctx->lock);
    update(ctx);
}

extern uint64_t mach_absolute_time(void);

// Called locked
static inline void update_vo_params(struct vo *vo, struct mpv_render_context *ctx) {
    vo->dwidth = ctx->vp_w;
    vo->dheight = ctx->vp_h;
    vo->monitor_par = ctx->vp_par;
}

static void flip_page(struct vo *vo)
{
    struct vo_priv *p = vo->priv;
    struct mpv_render_context *ctx = p->ctx;

    pthread_mutex_lock(&ctx->lock);

    struct timespec ts = mp_rel_time_to_timespec(0.2);
    // Wait until frame was rendered
    while (ctx->next_frame && !ctx->shutting_down) {
        if (pthread_cond_timedwait(&ctx->video_wait, &ctx->lock, &ts)) {
            if (ctx->next_frame) {
                MP_VERBOSE(vo, "mpv_render_context_render() not being called "
                           "or stuck.\n");
                goto done;
            }
        }
    }

    // Unblock mpv_render_context_render().
    ctx->render_count += 1;
    pthread_cond_broadcast(&ctx->video_wait);
    update_vo_params(vo, ctx);

    if (ctx->redrawing)
        goto done; // do not block for redrawing

    uint64_t bef = ctx->last_presentation_time;
    uint64_t befvsync = ctx->last_vsync_time;

    uint64_t flush_bef = mach_absolute_time();

    while ((ctx->expected_flush_count > ctx->flush_count) && !ctx->shutting_down) {
        if (pthread_cond_timedwait(&ctx->video_wait, &ctx->lock, &ts)) {
            printf("Bail on flush\n");
            MP_VERBOSE(vo, "mpv_render_report_flush() not being called.\n");
            goto done;
        }
    }
    
    while ((ctx->expected_present_count > ctx->present_count) && !ctx->shutting_down) {
        if (pthread_cond_timedwait(&ctx->video_wait, &ctx->lock, &ts)) {
            printf("Bail on present\n");
            MP_VERBOSE(vo, "mpv_render_report_present() not being called.\n");
            goto done;
        }
    }
    

    uint64_t flush_aft = mach_absolute_time();
    uint64_t befvsync2 = ctx->last_vsync_time;

    uint64_t swap_aft = mach_absolute_time();
    ctx->last_presentation_time = ctx->last_vsync_time + ctx->pending_swap_count * ctx->vsync_interval;
    uint64_t aft = ctx->last_presentation_time;
    if (((int64_t) aft - (int64_t) bef) > 17000) {
        printf("vo_libmpv flip time %llu, aft %llu, bef %llu, vsync diff %llu, interval %llu\n", (aft - bef), aft, bef, ctx->last_vsync_time - befvsync, ctx->vsync_interval);
        printf("\tvo: flush diff %.1f, swap diff (host time) %.1f, swap diff (vsync time) %llu\n", (flush_aft - flush_bef)*(125.0/3)/1e3, (swap_aft - flush_aft)*(125.0/3)/1e3, ctx->last_vsync_time - befvsync2);
    }

done:

    // Cleanup after the API user is not reacting, or is being unusually slow.
    if (ctx->next_frame && !ctx->shutting_down) {
        printf("Render timeout\n");
        talloc_free(ctx->cur_frame);
        ctx->cur_frame = ctx->next_frame;
        ctx->next_frame = NULL;
        ctx->render_count += 2;
        ctx->last_presentation_time = 0;
        pthread_cond_signal(&ctx->video_wait);
        vo_increment_drop_count(vo, 1);
    }

    pthread_mutex_unlock(&ctx->lock);
}

static int query_format(struct vo *vo, int format)
{
    struct vo_priv *p = vo->priv;
    struct mpv_render_context *ctx = p->ctx;

    bool ok = false;
    pthread_mutex_lock(&ctx->lock);
    if (format >= IMGFMT_START && format < IMGFMT_END)
        ok = ctx->imgfmt_supported[format - IMGFMT_START];
    pthread_mutex_unlock(&ctx->lock);
    return ok;
}

static void run_control_on_render_thread(void *p)
{
    void **args = p;
    struct mpv_render_context *ctx = args[0];
    int request = (intptr_t)args[1];
    void *data = args[2];
    int ret = VO_NOTIMPL;

    switch (request) {
    case VOCTRL_SCREENSHOT: {
        pthread_mutex_lock(&ctx->lock);
        struct vo_frame *frame = vo_frame_ref(ctx->cur_frame);
        pthread_mutex_unlock(&ctx->lock);
        if (frame && ctx->renderer->fns->screenshot)
            ctx->renderer->fns->screenshot(ctx->renderer, frame, data);
        talloc_free(frame);
        break;
    }
    case VOCTRL_PERFORMANCE_DATA: {
        pthread_mutex_lock(&ctx->lock);
        pthread_mutex_unlock(&ctx->lock);
        if (ctx->renderer->fns->perfdata) {
            ctx->renderer->fns->perfdata(ctx->renderer, data);
            ret = VO_TRUE;
        }
        break;
    }
    }

    *(int *)args[3] = ret;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct vo_priv *p = vo->priv;
    struct mpv_render_context *ctx = p->ctx;

    switch (request) {
    case VOCTRL_RESET:
        pthread_mutex_lock(&ctx->lock);
        forget_frames(ctx, false);
        ctx->need_reset = true;
        pthread_mutex_unlock(&ctx->lock);
        vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_PAUSE:
        vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_REDRAW:
        vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_SET_PANSCAN:
        pthread_mutex_lock(&ctx->lock);
        ctx->need_resize = true;
        pthread_mutex_unlock(&ctx->lock);
        vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_UPDATE_RENDER_OPTS:
        pthread_mutex_lock(&ctx->lock);
        ctx->need_update_external = true;
        pthread_mutex_unlock(&ctx->lock);
        vo->want_redraw = true;
        return VO_TRUE;
    }

    // VOCTRLs to be run on the renderer thread (if possible at all).
   if (ctx->advanced_control) {
        switch (request) {
        case VOCTRL_SCREENSHOT:
        case VOCTRL_PERFORMANCE_DATA: {
            int ret;
            void *args[] = {ctx, (void *)(intptr_t)request, data, &ret};
            mp_dispatch_run(ctx->dispatch, run_control_on_render_thread, args);
            return ret;
        }
        }
    }

    int r = VO_NOTIMPL;
    pthread_mutex_lock(&ctx->control_lock);
    if (ctx->control_cb) {
        int events = 0;
        r = p->ctx->control_cb(vo, p->ctx->control_cb_ctx,
                               &events, request, data);
        // While this is mostly equivalent to user calling render(), this has the benefit
        // that it can coalesce repeated resizes
        if (events & VO_EVENT_RESIZE) {
            vo->want_redraw = true;
        }
        vo_event(vo, events);
    }
    pthread_mutex_unlock(&ctx->control_lock);

    return r;
}

static struct mp_image *get_image(struct vo *vo, int imgfmt, int w, int h,
                                  int stride_align)
{
    struct vo_priv *p = vo->priv;
    struct mpv_render_context *ctx = p->ctx;

    if (ctx->dr)
        return dr_helper_get_image(ctx->dr, imgfmt, w, h, stride_align);

    return NULL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct vo_priv *p = vo->priv;
    struct mpv_render_context *ctx = p->ctx;

    pthread_mutex_lock(&ctx->lock);
    forget_frames(ctx, true);
    ctx->img_params = *params;
    ctx->need_reconfig = true;
    ctx->need_resize = true;
    pthread_mutex_unlock(&ctx->lock);

    control(vo, VOCTRL_RECONFIG, NULL);

    return 0;
}

static void uninit(struct vo *vo)
{
    struct vo_priv *p = vo->priv;
    struct mpv_render_context *ctx = p->ctx;

    control(vo, VOCTRL_UNINIT, NULL);

    pthread_mutex_lock(&ctx->lock);

    forget_frames(ctx, true);
    ctx->img_params = (struct mp_image_params){0};
    ctx->need_reconfig = true;
    ctx->need_resize = true;
    ctx->need_update_external = true;
    ctx->need_reset = true;
    ctx->vo = NULL;

    // The following do not normally need ctx->lock, however, ctx itself may
    // become invalid once we release ctx->lock.
    bool prev_in_use = atomic_exchange(&ctx->in_use, false);
    assert(prev_in_use); // obviously must have been set
    mp_dispatch_interrupt(ctx->dispatch);

    pthread_mutex_unlock(&ctx->lock);
}

#import <pthread.h>

extern int cocoa_set_realtime(struct mp_log *log, double fraction);

static int preinit(struct vo *vo)
{
    struct vo_priv *p = vo->priv;

    struct mpv_render_context *ctx =
        mp_client_api_acquire_render_context(vo->global->client_api);
    p->ctx = ctx;

    if (!ctx) {
        if (!vo->probing)
            MP_FATAL(vo, "No render context set.\n");
        else
            MP_VERBOSE(vo, "No render context for libmpv found while probing.\n");
        return -1;
    }

    control(vo, VOCTRL_PREINIT, NULL);
    // After this, we must have a valid context. Use presence of dispatch as signal.
    pthread_mutex_lock(&ctx->lock);
    assert(ctx->dispatch);
    ctx->vo = vo;
    ctx->need_resize = true;
    ctx->need_update_external = true;
    pthread_mutex_unlock(&ctx->lock);

    vo->hwdec_devs = ctx->hwdec_devs;
    
    cocoa_set_realtime(vo->log, 0.2);

    return 0;
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct vo_priv *p = vo->priv;
    struct mpv_render_context *ctx = p->ctx;

    if (ctx->last_presentation_time == 0) {
        return;
    }
    info->last_queue_display_time = ctx->last_presentation_time;
}

const struct vo_driver video_out_libmpv = {
    .description = "render API for libmpv",
    .name = "libmpv",
    .caps = VO_CAP_ROTATE90,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .get_vsync = get_vsync,
    .get_image_ts = get_image,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct vo_priv),
};
