/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include <assert.h>

#include "config.h"
#include "mpv_talloc.h"

#include "common/msg.h"
#include "options/options.h"
#include "common/common.h"
#include "common/global.h"

#include "stream/stream.h"
#include "sub/dec_sub.h"
#include "demux/demux.h"
#include "video/mp_image.h"

#include "core.h"

// 0: primary sub, 1: secondary sub, -1: not selected
static int get_order(struct MPContext *mpctx, struct track *track)
{
    for (int n = 0; n < NUM_PTRACKS; n++) {
        if (mpctx->current_track[n][STREAM_SUB] == track)
            return n;
    }
    return -1;
}

static void reset_subtitles(struct MPContext *mpctx, struct track *track)
{
    if (track->d_sub)
        sub_reset(track->d_sub);
    term_osd_set_subs(mpctx, NULL);
}

void reset_subtitle_state(struct MPContext *mpctx)
{
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct dec_sub *d_sub = mpctx->tracks[n]->d_sub;
        if (d_sub)
            sub_reset(d_sub);
    }
    term_osd_set_subs(mpctx, NULL);
}

void uninit_sub(struct MPContext *mpctx, struct track *track)
{
    if (track && track->d_sub) {
        reset_subtitles(mpctx, track);
        sub_select(track->d_sub, false);
        int order = get_order(mpctx, track);
        osd_set_sub(mpctx->osd, order, NULL);
    }
}

void uninit_sub_all(struct MPContext *mpctx)
{
    mpctx->force_sub_update = false;
    for (int n = 0; n < mpctx->num_tracks; n++)
        uninit_sub(mpctx, mpctx->tracks[n]);
}

// If force-readahead is set, try to read-ahead packets even for a lazy stream.
static bool update_subtitle(struct MPContext *mpctx, double video_pts,
                            struct track *track, bool force_readahead)
{
    struct dec_sub *dec_sub = track ? track->d_sub : NULL;

    if (!dec_sub || video_pts == MP_NOPTS_VALUE || !track->selected)
        return true;

    if (mpctx->vo_chain) {
        struct mp_image_params params = mpctx->vo_chain->filter->input_params;
        if (params.imgfmt)
            sub_control(dec_sub, SD_CTRL_SET_VIDEO_PARAMS, &params);
    }

    // Though preload uses a blocking packet read call, this should
    // not actually block "fully_read" implies the demuxer (libavformat)
    // has contents completely in-memory.
    if (track->demuxer->fully_read && sub_can_preload(dec_sub)) {
        // Assume fully_read implies no interleaved audio/video streams.
        // (Reading packets will change the demuxer position.)
        demux_seek(track->demuxer, 0, 0);
        sub_preload(dec_sub);
    }

    // In the case that we track switch for a lazy stream when mpv is paused, 
    // even though the "refresh seek" mechanism will try to read ahead until
    // we catch up to the old video position, this can take time and if we
    // consume all the packets before that is done we can get signaled eof for
    // a lazy stream so we will exit early. In reality however we need to keep
    // retrying, so we re-use the forced min_pts read mechanism to indicate
    // that we should not return eof until video_pts is reached.
    if (!sub_read_packets(dec_sub, video_pts, force_readahead))
        return false;

    // Handle displaying subtitles on terminal; never done for secondary subs
    if (mpctx->current_track[0][STREAM_SUB] == track && !mpctx->video_out)
        term_osd_set_subs(mpctx, sub_get_text(dec_sub, video_pts));

    return true;
}

// Return true if the subtitles for the given PTS are ready; false if the player
// should wait for new demuxer data, and then should retry.
bool update_subtitles(struct MPContext *mpctx, double video_pts, bool force_readahead) {
    bool ok = true;
    for (int n = 0; n < NUM_PTRACKS; n++) {
        struct track *track = mpctx->current_track[n][STREAM_SUB];
        ok &= update_subtitle(mpctx, video_pts, track, force_readahead);
    }

    if (ok && mpctx->video_out) {
        // Handle displaying subtitles on VO with no video being played. This is
        // quite differently, because normally subtitles are redrawn on new video
        // frames, using the video frames' timestamps.
        if (mpctx->video_status == STATUS_EOF && osd_get_force_video_pts(mpctx->osd) != video_pts) {
            osd_set_force_video_pts(mpctx->osd, video_pts);
            osd_query_and_reset_want_redraw(mpctx->osd);
            vo_redraw(mpctx->video_out);
            // Force an arbitrary minimum FPS
            mp_set_timeout(mpctx, 0.1);
        } else if (force_readahead && mpctx->paused) {
            // Similarly if we force-update subtitles when paused,
            // we need to queue a vo redraw to be able to see it.
            printf("Force VO Redraw\n");
            vo_redraw(mpctx->video_out);
        }
    }

    if (mpctx->paused) {
        printf("Called update sub when paused. Forced %d, Ret %d\n", force_readahead, ok);
    }
    return ok;
}

static struct attachment_list *get_all_attachments(struct MPContext *mpctx)
{
    struct attachment_list *list = talloc_zero(NULL, struct attachment_list);
    struct demuxer *prev_demuxer = NULL;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *t = mpctx->tracks[n];
        if (!t->demuxer || prev_demuxer == t->demuxer)
            continue;
        prev_demuxer = t->demuxer;
        for (int i = 0; i < t->demuxer->num_attachments; i++) {
            struct demux_attachment *att = &t->demuxer->attachments[i];
            struct demux_attachment copy = {
                .name = talloc_strdup(list, att->name),
                .type = talloc_strdup(list, att->type),
                .data = talloc_memdup(list, att->data, att->data_size),
                .data_size = att->data_size,
            };
            MP_TARRAY_APPEND(list, list->entries, list->num_entries, copy);
        }
    }
    return list;
}

static bool init_subdec(struct MPContext *mpctx, struct track *track)
{
    assert(!track->d_sub);

    if (!track->demuxer || !track->stream)
        return false;

    track->d_sub = sub_create(mpctx->global, track->stream,
                              get_all_attachments(mpctx));
    if (!track->d_sub)
        return false;

    struct track *vtrack = mpctx->current_track[0][STREAM_VIDEO];
    struct mp_codec_params *v_c =
        vtrack && vtrack->stream ? vtrack->stream->codec : NULL;
    double fps = v_c ? v_c->fps : 25;
    sub_control(track->d_sub, SD_CTRL_SET_VIDEO_DEF_FPS, &fps);

    return true;
}

void reinit_sub(struct MPContext *mpctx, struct track *track)
{
    if (!track || !track->stream || track->stream->type != STREAM_SUB)
        return;

    if (!track->d_sub && !init_subdec(mpctx, track)) {
        error_on_track(mpctx, track);
        return;
    }

    sub_select(track->d_sub, true);
    int order = get_order(mpctx, track);
    osd_set_sub(mpctx->osd, order, track->d_sub);
    sub_control(track->d_sub, SD_CTRL_SET_TOP, &(bool){!!order});

    // When track switching when paused, we want to force-fetch subtitles.
    // Note that we don't have to explicitly take care of paused for cache case
    // (where reads might block for a long-time) since we don't allow is_streaming
    // tracks to be force-read.
    if (mpctx->playback_initialized) {
        bool force_readahead = mpctx->paused;
        bool ok = update_subtitles(mpctx, mpctx->playback_pts, force_readahead);
        // If we need to retry later, signal playloop to call update_subtitles again.
        mpctx->force_sub_update = force_readahead && !ok;
    }
  
}

void reinit_sub_all(struct MPContext *mpctx)
{
    for (int n = 0; n < NUM_PTRACKS; n++)
        reinit_sub(mpctx, mpctx->current_track[n][STREAM_SUB]);
}
