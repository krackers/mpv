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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#include <math.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "config.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "mpv_talloc.h"
#include "common/msg.h"
#include "common/global.h"
#include "osdep/atomic.h"
#include "osdep/threads.h"

#include "stream/stream.h"
#include "demux.h"
#include "timeline.h"
#include "stheader.h"
#include "cue.h"

#pragma clang diagnostic error "-Wfloat-conversion"

// Demuxer list
extern const struct demuxer_desc demuxer_desc_edl;
extern const struct demuxer_desc demuxer_desc_cue;
extern const demuxer_desc_t demuxer_desc_rawaudio;
extern const demuxer_desc_t demuxer_desc_rawvideo;
extern const demuxer_desc_t demuxer_desc_tv;
extern const demuxer_desc_t demuxer_desc_mf;
extern const demuxer_desc_t demuxer_desc_matroska;
extern const demuxer_desc_t demuxer_desc_lavf;
extern const demuxer_desc_t demuxer_desc_playlist;
extern const demuxer_desc_t demuxer_desc_disc;
extern const demuxer_desc_t demuxer_desc_rar;
extern const demuxer_desc_t demuxer_desc_libarchive;
extern const demuxer_desc_t demuxer_desc_null;
extern const demuxer_desc_t demuxer_desc_timeline;

/* Please do not add any new demuxers here. If you want to implement a new
 * demuxer, add it to libavformat, except for wrappers around external
 * libraries and demuxers requiring binary support. */

const demuxer_desc_t *const demuxer_list[] = {
    &demuxer_desc_disc,
    &demuxer_desc_edl,
    &demuxer_desc_cue,
    &demuxer_desc_rawaudio,
    &demuxer_desc_rawvideo,
#if HAVE_TV
    &demuxer_desc_tv,
#endif
    &demuxer_desc_matroska,
#if HAVE_LIBARCHIVE
    &demuxer_desc_libarchive,
#endif
    &demuxer_desc_rar,
    &demuxer_desc_lavf,
    &demuxer_desc_mf,
    &demuxer_desc_playlist,
    &demuxer_desc_null,
    NULL
};

struct demux_opts {
    int64_t max_bytes;
    int64_t max_bytes_bw;
    double min_secs;
    int force_seekable;
    double min_secs_cache;
    int demux_cache;
    int access_references;
    int seekable_cache;
    int create_ccs;
};

#define OPT_BASE_STRUCT struct demux_opts

const struct m_sub_options demux_conf = {
    .opts = (const struct m_option[]){
        OPT_DOUBLE("demuxer-readahead-secs", min_secs, M_OPT_MIN, .min = 0),
        OPT_BYTE_SIZE("demuxer-max-bytes", max_bytes, 0, 0, INT_MAX),
        OPT_BYTE_SIZE("demuxer-max-back-bytes", max_bytes_bw, 0, 0, INT_MAX),
        OPT_FLAG("force-seekable", force_seekable, 0),
        OPT_DOUBLE("demuxer-cache-secs", min_secs_cache, M_OPT_MIN, .min = 0),
        OPT_FLAG("access-references", access_references, 0),
        OPT_CHOICE("demuxer-seekable-cache", seekable_cache, 0,
                   ({"auto", -1}, {"no", 0}, {"yes", 1})),
        OPT_CHOICE("demuxer-cache", demux_cache, 0,
                   ({"auto", -1}, {"no", 0}, {"yes", 1})),
        OPT_FLAG("sub-create-cc-track", create_ccs, 0),
        {0}
    },
    .size = sizeof(struct demux_opts),
    .defaults = &(const struct demux_opts){
        .max_bytes = 150 * 1024 * 1024,
        .max_bytes_bw = 50 * 1024 * 1024,
        .min_secs = 1.0,
        .min_secs_cache = 10.0 * 60 * 60,
        .seekable_cache = -1,
        .demux_cache = -1,
        .access_references = 1,
    },
};

// Note that different streams can share the same internal demux state
// For instance if you're demuxing an mp4 with 1 video, 1 audio, and 1 sub stream
// All three would share the same internal state (because there's 1 demuxer demuxing packets for each)
struct demux_internal {
    struct mp_log *log;

    // The demuxer runs potentially in another thread, so we keep two demuxer
    // structs; the real demuxer can access the shadow struct only.
    struct demuxer *d_thread;   // accessed by demuxer impl. (producer)
    struct demuxer *d_user;     // accessed by player (consumer)

    // The lock protects the packet queues (struct demux_stream),
    // and the fields below.
    pthread_mutex_t lock;
    pthread_cond_t wakeup;
    pthread_t thread;

    // -- All the following fields are protected by lock.

    bool thread_terminate;
    bool threading;
    void (*wakeup_cb)(void *ctx);
    void *wakeup_cb_ctx;

    struct sh_stream **streams;
    int num_streams;

    // If non-NULL, a _selected_ stream which is used for global (timed)
    // metadata. It will be an arbitrary stream that is hopefully not sparse
    // (i.e. not a subtitle stream). This is needed because due to variable
    // interleaving multiple streams won't agree whether timed metadata is in
    // effect yet at the same time position.
    struct demux_stream *master_stream;

    int events;

    bool warned_queue_overflow;
    bool last_eof;              // last actual global EOF status
    bool eof;                   // whether we're in EOF state (reset for retry)
    bool idle;
    bool autoselect;
    double min_secs;
    int max_bytes;
    int max_bytes_bw;
    bool seekable_cache;

    // At least one decoder actually requested data since init or the last seek.
    // Do this to allow the decoder thread to select streams before starting.
    bool reading;

    // Set if we just performed a seek, without reading packets yet. Used to
    // avoid a redundant initial seek after enabling streams. We could just
    // allow it, but to avoid buggy seeking affecting normal playback, we don't.
    bool after_seek;
    bool after_seek_to_start;

    bool tracks_switched;       // thread needs to inform demuxer of this

    bool seeking;               // there's a seek queued
    int seek_flags;             // flags for next seek (if seeking==true)
    double seek_pts;

    // (fields for debugging)
    double seeking_in_progress; // low level seek state
    int low_level_seeks;        // number of started low level seeks
    double demux_ts;            // last demuxed DTS or PTS. (across all streams handled by the demuxer!)

    double ts_offset;           // timestamp offset to apply to everything

    void (*run_fn)(void *);     // if non-NULL, function queued to be run on
    void *run_fn_arg;           // the thread as run_fn(run_fn_arg)

    // (sorted by least recent use: index 0 is least recently used)
    struct demux_cached_range **ranges;
    int num_ranges;

    size_t total_bytes;         // total sum of packet data buffered

    // Range from which decoder is reading, and to which demuxer is appending.
    // This is never NULL. This is always ranges[num_ranges - 1].
    struct demux_cached_range *current_range;

    double highest_av_pts;      // highest non-subtitle PTS seen - for duration

    bool blocked;

    // Transient state.
    double duration;
    // Cached state.
    bool force_cache_update;
    struct stream_cache_info stream_cache_info;
    int64_t stream_size;
    // Updated during init only.
    char *stream_base_filename;
};

// A continuous range of cached packets for all enabled streams.
// (One demux_queue for each known stream.)
struct demux_cached_range {
    // streams[] is indexed by demux_stream->index
    struct demux_queue **streams;
    int num_streams;

    // Computed from the stream queue's values. These fields (unlike as with
    // demux_queue) are always either NOPTS, or fully valid (represent a valid
    // inclusive pts range between [seek_start, seek_end] across all queues
    // in the range. See the detailed comments about how it takes into account
    // sparse stream behavior.
    double seek_start, seek_end;

    bool is_bof;            // set if the file begins with this range
    bool is_eof;            // set if the file ends with this range
};

#define MAX_INDEX_ENTRIES 128

// A continuous list of cached packets for a single stream/range. There is one
// for each stream and range. Also contains some state for use during demuxing
// (keeping it across seeks makes it easier to resume demuxing).
struct demux_queue {
    struct demux_stream *ds;
    struct demux_cached_range *range;

    struct demux_packet *head;
    struct demux_packet *tail;

    struct demux_packet *next_prune_target; // cached value for faster pruning

    uint64_t tail_cum_pos;  // cumulative size including tail packet

    bool correct_dts;       // packet DTS is strictly monotonically increasing
    bool correct_pos;       // packet pos is strictly monotonically increasing
    int64_t last_pos;       // for determining correct_pos
    int64_t last_pos_fixup; // for filling in unset dp->pos values
    double last_dts;        // for determining correct_dts
    double last_ts;         // timestamp of the last packet added to queue

    // for incrementally determining seek PTS range
    // Represents min/max pts between keyframe_latest and latest (tail) packet.
    double inter_kf_min_pts, inter_kf_max_pts;
    struct demux_packet *keyframe_latest; // This may be set to null if EOF is reached
    struct demux_packet *keyframe_earliest;

    // incrementally maintained seek range, possibly representing an empty range
    // seek_start is the min_pts between first and second keyframe
    // seek_end is the max_pts between second-to-last and last keyframe
    double seek_start, seek_end;
    double last_pruned;     // timestamp of last pruned keyframe

    bool is_bof;            // started demuxing at beginning of file
    bool is_eof;            // received true EOF here

    // incomplete index to somewhat speed up seek operations
    // the entries in index[] must be in packet queue append/removal order
    // Entries are guaranteed to be sorted, with entry i+1 of pts at 
    // least index_distance from entry i
    int num_index;          // valid index[] entries
    double index_distance;  // minimum keyframe distance to add index element

    // TODO: This should be a ring buffer instead of an array. See upstream mpv.
    struct demux_packet *index[MAX_INDEX_ENTRIES];
};

struct demux_stream {
    struct demux_internal *in;
    struct sh_stream *sh;   // ds->sh->ds == ds
    enum stream_type type;  // equals to sh->type
    int index;              // equals to sh->index
    // --- all fields are protected by in->lock

    void (*wakeup_cb)(void *ctx);
    void *wakeup_cb_ctx;

    // demuxer state
    bool selected;          // user wants packets from this stream
    bool eager;             // try to keep at least 1 packet queued
                            // if false, this stream is disabled, or passively
                            // read (like subtitles). Note that subtitles
                            // are treated as sparse since they are "sparse" streams
                            // and if we tried to keep at least 1 packet queued
                            // we'd end up needing to queu a lot of av packets.
    bool still_image;       // stream has still video images
    bool refreshing;        // finding old position after track switches
    bool eof;               // end of demuxed stream? (true if no more packets)
                            // Note that unlike upstream mpv this will be false even for "lazy"
                            // streams unless we can truly never expect to see any more packets.

    bool global_correct_dts;// all observed so far
    bool global_correct_pos;

    // current queue - used both for reading and demuxing (this is never NULL)
    struct demux_queue *queue;

    // reader (decoder) state (bitrate calculations are part of it because we
    // want to return the bitrate closest to the "current position")
    double base_ts;         // timestamp of the last packet returned to decoder
    double last_br_ts;      // timestamp of last packet bitrate was calculated
    size_t last_br_bytes;   // summed packet sizes since last bitrate calculation
    double bitrate;
    struct demux_packet *reader_head;   // points at current decoder position
    bool skip_to_keyframe;
    bool attached_picture_added;
    bool need_wakeup;       // call wakeup_cb on next reader_head state change
    double force_read_until;// eager=false streams (subs): force read-ahead


    // for refresh seeks: pos/dts of last packet returned to reader
    int64_t last_ret_pos;
    double last_ret_dts;

    // for closed captions (demuxer_feed_caption)
    struct sh_stream *cc;
    bool ignore_eof;        // ignore stream in underrun detection

    // timed metadata
    struct mp_packet_tags *tags_demux;  // demuxer state (last updated metadata)
    struct mp_packet_tags *tags_reader; // reader state (last returned packet)
    struct mp_packet_tags *tags_init;   // global state at start of demuxing
};

// "Snapshot" of the tag state. Refcounted to avoid a copy per packet.
struct mp_packet_tags {
    mp_atomic_int64 refcount;
    struct mp_tags *demux;      // demuxer global tags (normal thing)
    struct mp_tags *stream;     // byte stream tags (ICY crap)
    struct mp_tags *sh;         // per sh_stream tags (e.g. OGG)
};

// Return "a", or if that is NOPTS, return "def".
#define PTS_OR_DEF(a, def) ((a) == MP_NOPTS_VALUE ? (def) : (a))
// If one of the values is NOPTS, always pick the other one.
#define MP_PTS_MIN(a, b) MPMIN(PTS_OR_DEF(a, b), PTS_OR_DEF(b, a))
#define MP_PTS_MAX(a, b) MPMAX(PTS_OR_DEF(a, b), PTS_OR_DEF(b, a))

#define MP_ADD_PTS(a, b) ((a) == MP_NOPTS_VALUE ? (a) : ((a) + (b)))

static void demuxer_sort_chapters(demuxer_t *demuxer);
static void *demux_thread(void *pctx);
static void update_cache(struct demux_internal *in);

static uint64_t get_foward_buffered_bytes(struct demux_stream *ds)
{
    if (!ds->reader_head)
        return 0;
    return ds->queue->tail_cum_pos - ds->reader_head->cum_pos;
}

void mp_packet_tags_unref(struct mp_packet_tags *tags)
{
    if (tags) {
        if (atomic_fetch_add(&tags->refcount, -1) == 1) {
            talloc_free(tags->sh);
            talloc_free(tags->demux);
            talloc_free(tags->stream);
            talloc_free(tags);
        }
    }
}

void mp_packet_tags_setref(struct mp_packet_tags **dst, struct mp_packet_tags *src)
{
    if (src)
        atomic_fetch_add(&src->refcount, 1);
    mp_packet_tags_unref(*dst);
    *dst = src;
}

static struct mp_tags *tags_dup_or_null(struct mp_tags *t)
{
    return t ? mp_tags_dup(NULL, t) : talloc_zero(NULL, struct mp_tags);
}

// Return a "deep" copy. If tags==NULL, allocate a new one.
static struct mp_packet_tags *mp_packet_tags_copy(struct mp_packet_tags *tags)
{
    struct mp_packet_tags *new = talloc_ptrtype(NULL, new);
    *new = (struct mp_packet_tags){
        .refcount = ATOMIC_VAR_INIT(1),
        .demux = tags_dup_or_null(tags ? tags->demux : NULL),
        .stream = tags_dup_or_null(tags ? tags->stream : NULL),
        .sh = tags_dup_or_null(tags ? tags->sh : NULL),
    };
    return new;
}

// Force a copy if refcount != 1.
// (refcount==1 means we're the unambiguous owner.)
// If *tags==NULL, allocate a blank one.
static void mp_packet_tags_make_writable(struct mp_packet_tags **tags)
{
    if (*tags && atomic_load(&(*tags)->refcount) == 1)
        return;
    struct mp_packet_tags *new = mp_packet_tags_copy(*tags);
    mp_packet_tags_unref(*tags);
    *tags = new;
}


// (this doesn't do most required things for a switch, like updating ds->queue)
static void set_current_range(struct demux_internal *in,
                              struct demux_cached_range *range)
{
    in->current_range = range;

    // Move to in->ranges[in->num_ranges-1] (for LRU sorting/invariant)
    for (int n = 0; n < in->num_ranges; n++) {
        if (in->ranges[n] == range) {
            MP_TARRAY_REMOVE_AT(in->ranges, in->num_ranges, n);
            break;
        }
    }
    MP_TARRAY_APPEND(in, in->ranges, in->num_ranges, range);
}

// Refresh range->seek_start/end.
static void update_seek_ranges(struct demux_cached_range *range)
{
    range->seek_start = range->seek_end = MP_NOPTS_VALUE;
    range->is_bof = true;
    range->is_eof = true;

    for (int n = 0; n < range->num_streams; n++) {
        struct demux_queue *queue = range->streams[n];

        // Sanity check: seek_start by construction should be seek_pts of the earliest keyframe.
        assert((queue->keyframe_earliest && queue->keyframe_earliest->kf_seek_pts == queue->seek_start)
                || (queue->seek_start == MP_NOPTS_VALUE));
        // cache being non-empty implies we should have earliest keyframe set.
        // note that seek_start might be unset though, see explanation in range joining.
        assert(queue->keyframe_earliest || queue->num_index == 0);
        // Must have a latest keyframe if we have an earliest keyframe.
        assert(queue->keyframe_latest || !queue->keyframe_earliest);

        if (queue->ds->selected && queue->ds->eager) {
            range->seek_start = MP_PTS_MAX(range->seek_start, queue->seek_start);
            range->seek_end = MP_PTS_MIN(range->seek_end, queue->seek_end);

            range->is_eof &= queue->is_eof;
            range->is_bof &= queue->is_bof;

            // If the seek range is the empty set
            // (either both nopts or same pts, then we can't use this range.)
            if (queue->seek_start >= queue->seek_end) {
                range->seek_start = range->seek_end = MP_NOPTS_VALUE;
                break;
            }
        }
    }

    // Sparse stream behavior is not very clearly defined, but usually we don't
    // want it to restrict the range of other streams, unless
    // This is incorrect in any of these cases:
    //  - sparse streams only (it's unknown how to determine an accurate range)
    //  - if sparse streams have non-keyframe packets (we set queue->last_pruned
    //    to the start of the pruned keyframe range - we'd need the end or so)
    // We also assume that ds->eager equals to a stream being sparse (usually
    // true, except if only sparse streams are selected).
    // We also rely on the fact that the demuxer position will always be ahead
    // of the seek_end for audio/video, because they need to prefetch at least
    // 1 packet to detect the end of a keyframe range. This means that we're
    // relatively guaranteed to have all sparse (subtitle) packets within the
    // seekable range.
    for (int n = 0; n < range->num_streams; n++) {
        struct demux_queue *queue = range->streams[n];
        if (queue->ds->selected && !queue->ds->eager &&
            queue->last_pruned != MP_NOPTS_VALUE &&
            range->seek_start != MP_NOPTS_VALUE)
        {
            // (last_pruned is _exclusive_ to the seekable range, so add a small
            // value to exclude it from the valid range.)
            range->seek_start =
                MP_PTS_MAX(range->seek_start, queue->last_pruned + 0.1);
        }
    }

    if (range->seek_start >= range->seek_end)
        range->seek_start = range->seek_end = MP_NOPTS_VALUE;
}

// Remove queue->head from the queue.
static void remove_head_packet(struct demux_queue *queue)
{
    struct demux_packet *dp = queue->head;

    assert(queue->ds->reader_head != dp);
    if (queue->next_prune_target == dp)
        queue->next_prune_target = NULL;
    if (queue->keyframe_latest == dp)
        queue->keyframe_latest = NULL;
    if (queue->keyframe_earliest == dp)
        queue->keyframe_earliest = NULL;
    queue->is_bof = false;

    uint64_t end_pos = dp->next ? dp->next->cum_pos : queue->tail_cum_pos;
    queue->ds->in->total_bytes -= end_pos - dp->cum_pos;

    if (queue->num_index && queue->index[0] == dp)
        MP_TARRAY_REMOVE_AT(queue->index, queue->num_index, 0);

    queue->head = dp->next;
    if (!queue->head)
        queue->tail = NULL;

    talloc_free(dp);
}

static void clear_queue(struct demux_queue *queue)
{
    struct demux_stream *ds = queue->ds;
    struct demux_internal *in = ds->in;

    if (queue->head)
        in->total_bytes -= queue->tail_cum_pos - queue->head->cum_pos;

    struct demux_packet *dp = queue->head;
    while (dp) {
        struct demux_packet *dn = dp->next;
        assert(ds->reader_head != dp);
        talloc_free(dp);
        dp = dn;
    }
    queue->head = queue->tail = NULL;
    queue->next_prune_target = NULL;
    queue->keyframe_latest = queue->keyframe_earliest = NULL;
    queue->seek_start = queue->seek_end = queue->last_pruned = MP_NOPTS_VALUE;

    queue->num_index = 0;
    queue->index_distance = 1.0;

    queue->correct_dts = queue->correct_pos = true;
    queue->last_pos = -1;
    queue->last_ts = queue->last_dts = MP_NOPTS_VALUE;
    queue->last_pos_fixup = -1;
    queue->inter_kf_min_pts = queue->inter_kf_max_pts = MP_NOPTS_VALUE;

    queue->is_eof = false;
    queue->is_bof = false;
}

static void clear_cached_range(struct demux_internal *in,
                               struct demux_cached_range *range)
{
    for (int n = 0; n < range->num_streams; n++)
        clear_queue(range->streams[n]);
    update_seek_ranges(range);
}

// Remove ranges with no data (except in->current_range). Also remove excessive
// ranges.
static void free_empty_cached_ranges(struct demux_internal *in)
{
    assert(in->current_range && in->num_ranges > 0);
    assert(in->current_range == in->ranges[in->num_ranges - 1]);

    while (1) {
        struct demux_cached_range *worst = NULL;

        for (int n = in->num_ranges - 2; n >= 0; n--) {
            struct demux_cached_range *range = in->ranges[n];
            if (range->seek_start == MP_NOPTS_VALUE || !in->seekable_cache) {
                clear_cached_range(in, range);
                MP_TARRAY_REMOVE_AT(in->ranges, in->num_ranges, n);
            } else {
                if (!worst || (range->seek_end - range->seek_start <
                               worst->seek_end - worst->seek_start))
                    worst = range;
            }
        }

        if (in->num_ranges <= MAX_SEEK_RANGES)
            break;

        clear_cached_range(in, worst);
    }
}

static void ds_clear_reader_queue_state(struct demux_stream *ds)
{
    ds->reader_head = NULL;
    ds->eof = false;
    ds->need_wakeup = true;
}

static void ds_clear_reader_state(struct demux_stream *ds)
{
    ds_clear_reader_queue_state(ds);

    ds->base_ts = ds->last_br_ts = MP_NOPTS_VALUE;
    ds->last_br_bytes = 0;
    ds->bitrate = -1;
    ds->skip_to_keyframe = false;
    ds->attached_picture_added = false;
    ds->last_ret_pos = -1;
    ds->last_ret_dts = MP_NOPTS_VALUE;
    ds->force_read_until = MP_NOPTS_VALUE;
}

// Call if the observed reader state on this stream somehow changes. The wakeup
// is skipped if the reader successfully read a packet, because that means we
// expect it to come back and ask for more.
static void wakeup_ds(struct demux_stream *ds)
{
    if (ds->need_wakeup) {
        if (ds->wakeup_cb) {
            ds->wakeup_cb(ds->wakeup_cb_ctx);
        } else if (ds->in->wakeup_cb) {
            ds->in->wakeup_cb(ds->in->wakeup_cb_ctx);
        }
        ds->need_wakeup = false;
        pthread_cond_signal(&ds->in->wakeup);
    }
}

static void update_stream_selection_state(struct demux_internal *in,
                                          struct demux_stream *ds)
{
    ds->eof = false;
    ds->refreshing = false;

    ds_clear_reader_state(ds);

    // We still have to go over the whole stream list to update ds->eager for
    // other streams too, because they depend on other stream's selections.

    bool any_av_streams = false;
    bool any_streams = false;
    struct demux_stream *master = NULL;

    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *s = in->streams[n]->ds;

        s->still_image = s->sh->still_image;
        s->eager = s->selected && !s->sh->attached_picture;
        if (s->eager && !s->still_image) {
            any_av_streams |= s->type != STREAM_SUB;
            if (!master ||
                (master->type == STREAM_VIDEO && s->type == STREAM_AUDIO))
            {
                master = s;
            }
        }
        any_streams |= s->selected;
    }

    in->master_stream = master;

    // Subtitles are only eagerly read if there are no other eagerly read
    // streams.
    if (any_av_streams) {
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *s = in->streams[n]->ds;

            if (s->type == STREAM_SUB)
                s->eager = false;
        }
    }

    if (!any_streams)
        in->blocked = false;

    // Make sure any stream reselection or addition is reflected in the seek
    // ranges, and also get rid of data that is not needed anymore (or
    // rather, which can't be kept consistent). This has to happen after we've
    // updated all the subtle state (like s->eager).
    for (int n = 0; n < in->num_ranges; n++) {
        struct demux_cached_range *range = in->ranges[n];

        if (!ds->selected)
            clear_queue(range->streams[ds->index]);

        update_seek_ranges(range);
    }

    free_empty_cached_ranges(in);

    wakeup_ds(ds);
}

void demux_set_ts_offset(struct demuxer *demuxer, double offset)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    in->ts_offset = offset;
    pthread_mutex_unlock(&in->lock);
}

static void add_missing_streams(struct demux_internal *in,
                                struct demux_cached_range *range)
{
    for (int n = range->num_streams; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;

        struct demux_queue *queue = talloc_ptrtype(range, queue);
        *queue = (struct demux_queue){
            .ds = ds,
            .range = range,
        };
        clear_queue(queue);
        MP_TARRAY_APPEND(range, range->streams, range->num_streams, queue);
        assert(range->streams[ds->index] == queue);
    }
}

// Allocate a new sh_stream of the given type. It either has to be released
// with talloc_free(), or added to a demuxer with demux_add_sh_stream(). You
// cannot add or read packets from the stream before it has been added.
struct sh_stream *demux_alloc_sh_stream(enum stream_type type)
{
    struct sh_stream *sh = talloc_ptrtype(NULL, sh);
    *sh = (struct sh_stream) {
        .type = type,
        .index = -1,
        .ff_index = -1,     // may be overwritten by demuxer
        .demuxer_id = -1,   // ... same
        .codec = talloc_zero(sh, struct mp_codec_params),
        .tags = talloc_zero(sh, struct mp_tags),
    };
    sh->codec->type = type;
    return sh;
}

static void ds_destroy(void *ptr)
{
    struct demux_stream *ds = ptr;
    mp_packet_tags_unref(ds->tags_init);
    mp_packet_tags_unref(ds->tags_reader);
    mp_packet_tags_unref(ds->tags_demux);
}

// Add a new sh_stream to the demuxer. Note that as soon as the stream has been
// added, it must be immutable, and must not be released (this will happen when
// the demuxer is destroyed).
static void demux_add_sh_stream_locked(struct demux_internal *in,
                                       struct sh_stream *sh)
{
    assert(!sh->ds); // must not be added yet

    sh->index = in->num_streams;

    sh->ds = talloc(sh, struct demux_stream);
    *sh->ds = (struct demux_stream) {
        .in = in,
        .sh = sh,
        .type = sh->type,
        .index = sh->index,
        .selected = in->autoselect,
        .global_correct_dts = true,
        .global_correct_pos = true,
    };
    talloc_set_destructor(sh->ds, ds_destroy);

    if (!sh->codec->codec)
        sh->codec->codec = "";

    if (sh->ff_index < 0)
        sh->ff_index = sh->index;
    if (sh->demuxer_id < 0) {
        sh->demuxer_id = 0;
        for (int n = 0; n < in->num_streams; n++) {
            if (in->streams[n]->type == sh->type)
                sh->demuxer_id += 1;
        }
    }

    MP_TARRAY_APPEND(in, in->streams, in->num_streams, sh);
    assert(in->streams[sh->index] == sh);

    for (int n = 0; n < in->num_ranges; n++)
        add_missing_streams(in, in->ranges[n]);

    sh->ds->queue = in->current_range->streams[sh->ds->index];

    update_stream_selection_state(in, sh->ds);

    mp_packet_tags_make_writable(&sh->ds->tags_init);
    mp_tags_replace(sh->ds->tags_init->demux, in->d_thread->metadata);
    mp_tags_replace(sh->ds->tags_init->sh, sh->tags);
    mp_packet_tags_setref(&sh->ds->tags_reader, sh->ds->tags_init);

    in->events |= DEMUX_EVENT_STREAMS;
    if (in->wakeup_cb)
        in->wakeup_cb(in->wakeup_cb_ctx);
}

// For demuxer implementations only.
void demux_add_sh_stream(struct demuxer *demuxer, struct sh_stream *sh)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_thread);
    pthread_mutex_lock(&in->lock);
    demux_add_sh_stream_locked(in, sh);
    pthread_mutex_unlock(&in->lock);
}

static void ds_modify_demux_tags(struct demux_stream *ds)
{
    if (!ds->tags_demux)
        mp_packet_tags_setref(&ds->tags_demux, ds->tags_init);
    mp_packet_tags_make_writable(&ds->tags_demux);
}

// Update sh->tags (lazily). This must be called by demuxers which update
// stream tags after init. (sh->tags can be accessed by the playback thread,
// which means the demuxer thread cannot write or read it directly.)
// Before init is finished, sh->tags can still be accessed freely.
// Ownership of tags goes to the function.
void demux_set_stream_tags(struct demuxer *demuxer, struct sh_stream *sh,
                           struct mp_tags *tags)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_thread);
    struct demux_stream *ds = sh->ds;
    assert(ds); // stream must have been added

    pthread_mutex_lock(&in->lock);

    ds_modify_demux_tags(ds);
    mp_tags_replace(ds->tags_demux->sh, tags);
    talloc_free(tags);

    pthread_mutex_unlock(&in->lock);
}

// Return a stream with the given index. Since streams can only be added during
// the lifetime of the demuxer, it is guaranteed that an index within the valid
// range [0, demux_get_num_stream()) always returns a valid sh_stream pointer,
// which will be valid until the demuxer is destroyed.
struct sh_stream *demux_get_stream(struct demuxer *demuxer, int index)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    assert(index >= 0 && index < in->num_streams);
    struct sh_stream *r = in->streams[index];
    pthread_mutex_unlock(&in->lock);
    return r;
}

// See demux_get_stream().
int demux_get_num_stream(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    int r = in->num_streams;
    pthread_mutex_unlock(&in->lock);
    return r;
}

void free_demuxer(demuxer_t *demuxer)
{
    if (!demuxer)
        return;
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    demux_stop_thread(demuxer);

    if (demuxer->desc->close)
        demuxer->desc->close(in->d_thread);

    demux_flush(demuxer);
    assert(in->total_bytes == 0);

    for (int n = 0; n < in->num_streams; n++)
        talloc_free(in->streams[n]);
    pthread_mutex_destroy(&in->lock);
    pthread_cond_destroy(&in->wakeup);
    talloc_free(demuxer);
}

void free_demuxer_and_stream(struct demuxer *demuxer)
{
    if (!demuxer)
        return;
    struct stream *s = demuxer->stream;
    free_demuxer(demuxer);
    free_stream(s);
}

// Start the demuxer thread, which reads ahead packets on its own.
void demux_start_thread(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    if (!in->threading) {
        in->threading = true;
        if (pthread_create(&in->thread, NULL, demux_thread, in))
            in->threading = false;
    }
}

void demux_stop_thread(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    if (in->threading) {
        pthread_mutex_lock(&in->lock);
        in->thread_terminate = true;
        pthread_cond_signal(&in->wakeup);
        pthread_mutex_unlock(&in->lock);
        pthread_join(in->thread, NULL);
        in->threading = false;
        in->thread_terminate = false;
    }
}

// The demuxer thread will call cb(ctx) if there's a new packet, or EOF is reached.
void demux_set_wakeup_cb(struct demuxer *demuxer, void (*cb)(void *ctx), void *ctx)
{
    struct demux_internal *in = demuxer->in;
    pthread_mutex_lock(&in->lock);
    assert(cb == NULL || !in->wakeup_cb);
    in->wakeup_cb = cb;
    in->wakeup_cb_ctx = ctx;
    pthread_mutex_unlock(&in->lock);
}

const char *stream_type_name(enum stream_type type)
{
    switch (type) {
    case STREAM_VIDEO:  return "video";
    case STREAM_AUDIO:  return "audio";
    case STREAM_SUB:    return "sub";
    default:            return "unknown";
    }
}

static struct sh_stream *demuxer_get_cc_track_locked(struct sh_stream *stream)
{
    struct sh_stream *sh = stream->ds->cc;

    if (!sh) {
        sh = demux_alloc_sh_stream(STREAM_SUB);
        if (!sh)
            return NULL;
        sh->codec->codec = "eia_608";
        sh->default_track = true;
        stream->ds->cc = sh;
        demux_add_sh_stream_locked(stream->ds->in, sh);
        sh->ds->ignore_eof = true;
    }

    return sh;
}

void demuxer_feed_caption(struct sh_stream *stream, demux_packet_t *dp)
{
    struct demux_internal *in = stream->ds->in;

    pthread_mutex_lock(&in->lock);
    struct sh_stream *sh = demuxer_get_cc_track_locked(stream);
    if (!sh) {
        pthread_mutex_unlock(&in->lock);
        talloc_free(dp);
        return;
    }

    dp->keyframe = true;
    dp->pts = MP_ADD_PTS(dp->pts, -in->ts_offset);
    dp->dts = MP_ADD_PTS(dp->dts, -in->ts_offset);
    pthread_mutex_unlock(&in->lock);

    demux_add_packet(sh, dp);
}

// Add the keyframe to the end of the index. Not all packets are actually added.
static void add_index_entry(struct demux_queue *queue, struct demux_packet *dp)
{
    assert(dp->keyframe && dp->kf_seek_pts != MP_NOPTS_VALUE);

    if (queue->num_index) {
        double prev = queue->index[queue->num_index - 1]->kf_seek_pts;
        if (dp->kf_seek_pts < prev + queue->index_distance)
            return;
    }

    while (queue->num_index == MAX_INDEX_ENTRIES) {
        // For case of video streams where keyframes may not be even
        // we can't directly just take every other index
        // but instead should keep indices with a distance that satisfies the new distance
        queue->index_distance *= 2;
        int current_index = 0;
        for (int i = 1; i < MAX_INDEX_ENTRIES; i++) {
            if (queue->index[i]->kf_seek_pts - queue->index[current_index]->kf_seek_pts >= queue->index_distance) {
                queue->index[++current_index] = queue->index[i];
            }
        }
        queue->num_index = current_index + 1;
    }

    queue->index[queue->num_index++] = dp;
}

// Check whether the next range in the list is, and if it appears to overlap,
// try joining it into a single range.
static void attempt_range_joining(struct demux_internal *in)
{
    struct demux_cached_range *next = NULL;
    double next_dist = INFINITY;

    assert(in->current_range && in->num_ranges > 0);
    assert(in->current_range == in->ranges[in->num_ranges - 1]);

    for (int n = 0; n < in->num_ranges - 1; n++) {
        struct demux_cached_range *range = in->ranges[n];

        if (in->current_range->seek_start <= range->seek_start) {
            // This uses ">" to get some non-0 overlap.
            double dist = in->current_range->seek_end - range->seek_start;
            if (dist > 0 && dist < next_dist) {
                next = range;
                next_dist = dist;
            }
        }
    }

    if (!next)
        return;

    MP_VERBOSE(in, "going to join ranges %f-%f + %f-%f\n",
               in->current_range->seek_start, in->current_range->seek_end,
               next->seek_start, next->seek_end);

    // Try to find a join point, where packets obviously overlap. (It would be
    // better and faster to do this incrementally, but probably too complex.)
    // The current range can overlap arbitrarily with the next one, not only by
    // by the seek overlap, but for arbitrary packet readahead as well.
    // We also drop the overlapping packets (if joining fails, we discard the
    // entire next range anyway, so this does no harm).
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;

        struct demux_queue *q1 = in->current_range->streams[n];
        struct demux_queue *q2 = next->streams[n];

        if (!ds->global_correct_pos && !ds->global_correct_dts) {
            MP_WARN(in, "stream %d: ranges unjoinable\n", n);
            goto failed;
        }

        struct demux_packet *end = q1->tail;
        bool join_point_found = !end; // no packets yet -> joining will work
        if (end) {
            while (q2->head) {
                struct demux_packet *dp = q2->head;

                // Some weird corner-case. We'd have to search the equivalent
                // packet in q1 to update it correctly. Better just give up.
                if (dp == q2->keyframe_latest) {
                    MP_VERBOSE(in, "stream %d: not enough keyframes for join\n", n);
                    goto failed;
                }

                if ((ds->global_correct_dts && dp->dts == end->dts) ||
                    (ds->global_correct_pos && dp->pos == end->pos))
                {
                    // Do some additional checks as a (imperfect) sanity check
                    // in case pos/dts are not "correct" across the ranges (we
                    // never actually check that).
                    if (dp->dts != end->dts || dp->pos != end->pos ||
                        dp->pts != end->pts || dp->len != end->len)
                    {
                        MP_WARN(in, "stream %d: weird demuxer behavior\n", n);
                        goto failed;
                    }

                    // q1 usually meets q2 at a keyframe. q1 will end on a key-
                    // frame (because it tries joining when reading a keyframe).
                    // Obviously, q1 can not know the kf_seek_pts yet; it would
                    // have to read packets after it to compute it. Ideally,
                    // we'd remove it and use q2's packet, but the linked list
                    // makes this hard, so copy this missing metadata instead.
                    end->kf_seek_pts = dp->kf_seek_pts;

                    remove_head_packet(q2);
                    join_point_found = true;
                    break;
                }

                // This happens if the next range misses the end packet. For
                // normal streams (ds->eager==true), this is a failure to find
                // an overlap. For subtitles, this can mean the current_range
                // has a subtitle somewhere before the end of its range, and
                // next has another subtitle somewhere after the start of its
                // range.
                if ((ds->global_correct_dts && dp->dts > end->dts) ||
                    (ds->global_correct_pos && dp->pos > end->pos))
                    break;

                remove_head_packet(q2);
            }
        }

        // For enabled non-sparse streams, always require an overlap packet.
        if (ds->eager && !join_point_found) {
            MP_WARN(in, "stream %d: no joint point found\n", n);
            goto failed;
        }
    }

    // Actually join the ranges. Now that we think it will work, mutate the
    // data associated with the current range.

    for (int n = 0; n < in->num_streams; n++) {
        struct demux_queue *q1 = in->current_range->streams[n];
        struct demux_queue *q2 = next->streams[n];

        struct demux_stream *ds = in->streams[n]->ds;
        assert(ds->queue == q1);

        // First new packet that is appended to the current range.
        struct demux_packet *join_point = q2->head;

        if (q2->head) {
            if (q1->head) {
                q1->tail->next = q2->head;
            } else {
                q1->head = q2->head;
            }
            q1->tail = q2->tail;
        }

        // Note that there are roughly 4 types of joins here, based on which we
        // may or may not end up with a keyframe_earliest and seek_start set for q1.
        // Let I indicate a keyframe (e.g. IDR for h264) and B indicate non-keyframe.
        // Let [x] indicate a shared overlap packet of x between q1 and q2.
        //
        //
        // We can have BB[I]BBBBI, which is the case talked about in the previous
        // section, and where we assign the kf_seek_pts to q1's end frame, so q1
        // will also have a valid seek_start and there are no discontinuities.
        //
        // We can also have IBB[B]BBIBBBI. In this case we cannot (easily) propagate the
        // kf_seek_pts info, so while q1 still has a keyframe_earliest, its kf_seek_pts
        // (and hence also seek_start) are unset. Q1 will still have the cache
        // entries transferred from q2 though. This is a rarer case (especially for
        // eager streams. For the purpose of range start/end computation, NOPTS values are ignored,
        // and if range joining was triggered by an eager stream at a keyframe boundary then
        // we should hopefully have at least one stream which has a defined seek_start which
        // will dictate the range). It does unfortunately mean that that earliest keyframe
        // will never be returned as a valid seek target (because it has no kf_seek_pts value)
        // but ideally this range is small enough that it's not worth worrying about.
        //
        // We can have BBB[B]IBBBIBB. In this case we take the keyframe_earliest from q2
        // which is the correct thing to do, and we'll set seek_start accordingly.
        //
        // BBB[B]BBB - no keyframe at all in seek range.

        // It might be the case (e.g. sparse stream) that q1 does not currently have any keyframe info, while q2 does.
        q1->keyframe_earliest = q1->keyframe_earliest ?: q2->keyframe_earliest;

        // Due to the assignment of end->kf_seek_pts, or the above line, we might now have a valid seek pts for
        // the earliest keyframe, in which case we should update seek_start to match.
        q1->seek_start = PTS_OR_DEF(q1->seek_start, q1->keyframe_earliest ?
                                                    q1->keyframe_earliest->kf_seek_pts : MP_NOPTS_VALUE);
        q1->seek_end = q2->seek_end;
        q1->correct_dts &= q2->correct_dts;
        q1->correct_pos &= q2->correct_pos;
        q1->last_pos = q2->last_pos;
        q1->last_dts = q2->last_dts;
        q1->last_ts = q2->last_ts;
        q1->inter_kf_min_pts = q2->inter_kf_min_pts;
        q1->inter_kf_max_pts = q2->inter_kf_max_pts;
        q1->keyframe_latest = q2->keyframe_latest;
        q1->is_eof = q2->is_eof;
        
        q1->last_pos_fixup = -1;

        q2->head = q2->tail = NULL;
        q2->next_prune_target = NULL;
        q2->keyframe_latest = q2->keyframe_earliest = NULL;

        for (int i = 0; i < q2->num_index; i++)
            add_index_entry(q1, q2->index[i]);
        q2->num_index = 0;

        if (ds->selected && !ds->reader_head)
            ds->reader_head = join_point;
        ds->skip_to_keyframe = false;

        // Make the cum_pos values in all q2 packets continuous.
        for (struct demux_packet *dp = join_point; dp; dp = dp->next) {
            uint64_t next_pos = dp->next ? dp->next->cum_pos : q2->tail_cum_pos;
            uint64_t size = next_pos - dp->cum_pos;
            dp->cum_pos = q1->tail_cum_pos;
            q1->tail_cum_pos += size;
        }

        // For moving demuxer position.
        ds->refreshing = ds->selected;
    }

     // N.b. only updating for range of q1, not q2
    update_seek_ranges(in->current_range);

    // Move demuxing position to after the current range.
    in->seeking = true;
    in->seek_flags = SEEK_HR;
    in->seek_pts = next->seek_end - 1.0;

    MP_VERBOSE(in, "ranges joined!\n");

failed:
    clear_cached_range(in, next);
    free_empty_cached_ranges(in);
}

// Determine seekable range when a packet is added. If dp==NULL, treat it as
// EOF (i.e. closes the current block).
// This has to deal with a number of corner cases, such as demuxers potentially
// starting output at non-keyframes.
// Can join seek ranges, which messes with in->current_range and all.
static void adjust_seek_range_on_packet(struct demux_stream *ds,
                                        struct demux_packet *dp)
{
    struct demux_queue *queue = ds->queue;
    bool attempt_range_join = false;
    bool prev_eof = queue->is_eof;

    if (!ds->in->seekable_cache)
        return;

    if (!dp || dp->keyframe) {
        if (queue->keyframe_latest) {
            queue->keyframe_latest->kf_seek_pts = queue->inter_kf_min_pts;
            double old_end = queue->range->seek_end;
            if (queue->seek_start == MP_NOPTS_VALUE)
                queue->seek_start = queue->inter_kf_min_pts;
            if (queue->inter_kf_max_pts != MP_NOPTS_VALUE)
                queue->seek_end = queue->inter_kf_max_pts;
            queue->is_eof = !dp;
            update_seek_ranges(queue->range);
            attempt_range_join = queue->range->seek_end > old_end;
            if (queue->keyframe_latest->kf_seek_pts != MP_NOPTS_VALUE)
                add_index_entry(queue, queue->keyframe_latest);
        } else {
            queue->is_eof |= ds->eof;
        }
        if (!queue->keyframe_earliest)
            queue->keyframe_earliest = dp; // Can be null if we are ending range.
        if (dp)
            queue->keyframe_latest = dp;
        queue->inter_kf_min_pts = queue->inter_kf_max_pts = MP_NOPTS_VALUE;
    }

    if (dp) {
        dp->kf_seek_pts = MP_NOPTS_VALUE;

        double ts = PTS_OR_DEF(dp->pts, dp->dts);
        if (dp->segmented && (ts < dp->start || ts > dp->end))
            ts = MP_NOPTS_VALUE;

        queue->inter_kf_min_pts = MP_PTS_MIN(queue->inter_kf_min_pts, ts);
        queue->inter_kf_max_pts = MP_PTS_MAX(queue->inter_kf_max_pts, ts);

        queue->is_eof = false;
    }

    if (queue->is_eof != prev_eof)
        update_seek_ranges(queue->range);

    if (attempt_range_join)
        attempt_range_joining(ds->in);
}

void demux_add_packet(struct sh_stream *stream, demux_packet_t *dp)
{
    struct demux_stream *ds = stream ? stream->ds : NULL;
    if (!dp || !dp->len || !ds || demux_cancel_test(ds->in->d_thread)) {
        talloc_free(dp);
        return;
    }
    struct demux_internal *in = ds->in;
    pthread_mutex_lock(&in->lock);

    in->after_seek = false;
    in->after_seek_to_start = false;

    double ts = dp->dts == MP_NOPTS_VALUE ? dp->pts : dp->dts;
    if (dp->segmented)
        ts = MP_PTS_MIN(ts, dp->end);

    if (ts != MP_NOPTS_VALUE)
        in->demux_ts = ts;

    struct demux_queue *queue = ds->queue;

    bool drop = !ds->selected || in->seeking || ds->sh->attached_picture;

    if (!drop) {
        // If libavformat splits packets, some packets will have pos unset, so
        // make up one based on the first packet => makes refresh seeks work.
        if ((dp->pos < 0 || dp->pos == queue->last_pos_fixup) && !dp->keyframe && queue->last_pos_fixup >= 0) {
            dp->pos = queue->last_pos_fixup + 1;
        }
        queue->last_pos_fixup = dp->pos;
    }

    if (!drop && ds->refreshing) {
        // Resume reading once the old position was reached (i.e. we start
        // returning packets where we left off before the refresh).
        // If it's the same position, drop, but continue normally next time.
        if (queue->correct_dts) {
            ds->refreshing = dp->dts < queue->last_dts;
        } else if (queue->correct_pos) {
            ds->refreshing = dp->pos < queue->last_pos;
        } else {
            ds->refreshing = false; // should not happen
            MP_WARN(in, "stream %d: demux refreshing failed\n", ds->index);
        }
        drop = true;
    }

    if (drop) {
        pthread_mutex_unlock(&in->lock);
        talloc_free(dp);
        return;
    }

    queue->correct_pos &= dp->pos >= 0 && dp->pos > queue->last_pos;
    queue->correct_dts &= dp->dts != MP_NOPTS_VALUE && dp->dts > queue->last_dts;
    queue->last_pos = dp->pos;
    queue->last_dts = dp->dts;
    ds->global_correct_pos &= queue->correct_pos;
    ds->global_correct_dts &= queue->correct_dts;

    dp->stream = stream->index;
    dp->next = NULL;
    mp_packet_tags_setref(&dp->metadata, ds->tags_demux);

    // (keep in mind that even if the reader went out of data, the queue is not
    // necessarily empty due to the backbuffer)
    if (!ds->reader_head && (!ds->skip_to_keyframe || dp->keyframe)) {
        ds->reader_head = dp;
        ds->skip_to_keyframe = false;
    }

    size_t bytes = demux_packet_estimate_total_size(dp);
    ds->in->total_bytes += bytes;
    dp->cum_pos = queue->tail_cum_pos;
    queue->tail_cum_pos += bytes;

    if (queue->tail) {
        // next packet in stream
        queue->tail->next = dp;
        queue->tail = dp;
    } else {
        // first packet in stream
        queue->head = queue->tail = dp;
    }

    if (!ds->ignore_eof) {
        // obviously not true anymore
        ds->eof = false;
        in->last_eof = in->eof = false;
    }

    // For video, PTS determination is not trivial, but for other media types
    // distinguishing PTS and DTS is not useful.
    if (stream->type != STREAM_VIDEO && dp->pts == MP_NOPTS_VALUE)
        dp->pts = dp->dts;

    if (ts != MP_NOPTS_VALUE && (ts > queue->last_ts || ts + 10 < queue->last_ts))
        queue->last_ts = ts;
    if (ds->base_ts == MP_NOPTS_VALUE)
        ds->base_ts = queue->last_ts;

    const char *num_pkts = queue->head == queue->tail ? "1" : ">1";
    uint64_t fw_bytes = get_foward_buffered_bytes(ds);
    MP_TRACE(in, "append packet to %s: size=%d pts=%f dts=%f pos=%"PRIi64" "
             "[num=%s size=%zd]\n", stream_type_name(stream->type),
             dp->len, dp->pts, dp->dts, dp->pos, num_pkts, (size_t) fw_bytes);

    adjust_seek_range_on_packet(ds, dp);

    // Possible update duration based on highest TS demuxed (but ignore subs).
    if (stream->type != STREAM_SUB) {
        if (dp->segmented)
            ts = MP_PTS_MIN(ts, dp->end);
        if (ts > in->highest_av_pts) {
            in->highest_av_pts = ts;
            double duration = in->highest_av_pts - in->d_thread->start_time;
            if (duration > in->d_thread->duration) {
                in->d_thread->duration = duration;
                // (Don't wakeup user thread, would be too noisy.)
                in->events |= DEMUX_EVENT_DURATION;
                in->duration = duration;
            }
        }
    }

    wakeup_ds(ds);
    pthread_mutex_unlock(&in->lock);
}

static void mark_stream_eof(struct demux_stream *ds)
{
    if (!ds->eof) {
        ds->eof = true;
        adjust_seek_range_on_packet(ds, NULL);
        wakeup_ds(ds);
    }
}

/**
 Mechanism to read-ahead packets (for lazy streams) until packet with force_read_until pts is reached.
 Note that if this mechanism is used with a forced pts far into the future, the demuxer will need to
 end up queueing all those intermediary bytes (the video/audio information), which can hit the demux
 forward byte limit. This occurs whether or not caching is used.
 In effect, it's like we "dynamically" extend the forward-readahead/packet caching range.
*/
// Read ahead at most 2 extra secs from desired pts, to simulate normal demux
// readahead behavior.
#define LAZY_WAIT_READAHEAD_PTS 2.0
static bool lazy_stream_force_read(struct demux_stream *ds)
{
    struct demux_internal *in = ds->in;
    // Attempt to read until force_read_until was reached, or true EOF.

    // Note: Unlike mentioned in 9d9e986e068fc47575ad93557ce9f92b5a992dae
    // I think it should actually work to have !ds->reader_head as
    // part of this condition. Reason is that if reader_head is set
    // then dequeue_packet will return that packet so ret code cannot be -1.
    // If the packet is not correct pts, then the sub code will read again, and now
    // if reader_head is unset then we force a read. So really the only
    // difference is that if reader_head is part of this condition we will wait
    // until all the existing packets in queue are drained before triggering
    // read ahead to min pts.
    // However, it still makes sense to me to avoid including that,
    // because if we did then we'd have to ping-pong back and forth between demuxer
    // and reader (since we'd never queue ahead more than a single packet, but with high probability
    // the packet we're looking for is several packets ahead. So it makes better sense
    // to just demux ahead to target so we can read in one swoop.)
    //
    // We don't handle queue overflow elegantly here; there's really no way we could,
    // because there are two conflicting conditions: we're trying to force a read to some pts
    // but we can't do that because the other stream queues are full so we can't add any more packets
    // to them (even if our sub queue is empty). There is code to prevent any more packets being
    // added and mark ds->eof as true (which will cause reader to stop trying), but lazy_stream_force_read
    // keeps returning true in such cases (because we still want to keep trying and continue force-readahead
    // as soon as we can). Note that including ds->reader_head in this condition wouldn't really help
    // with overflow case, as we'd still end up filling the non-sub queues with data. Maybe it might be a slight
    // optimization in that we will upper-bound the sub queue size as <= 1, but since sub packets are small
    // anyway it's not worth this optimization. So basically if you set readahead_pts too far into the future
    // a queue overflow _will_ occur and your subs probably won't show up on screen. Not much we can do
    // other than fail loudly by printing the queue overflow error message.
    //
    //
    // Also note that we don't include ds->selected here since it's the client
    // responsibility to check that, similar to eager case (if using demux_read then
    // this is done for you to avoid infinite looping.)
    return !ds->eager &&
           !in->last_eof && ds->force_read_until != MP_NOPTS_VALUE &&
           // A bit of a hack to avoid infinite readahead.
           (in->seeking || in->demux_ts == MP_NOPTS_VALUE || in->demux_ts <= ds->force_read_until + LAZY_WAIT_READAHEAD_PTS) &&
           // Stream queue lacks demuxed packet satisfying timestamp condition
           (ds->queue->last_ts == MP_NOPTS_VALUE || ds->queue->last_ts <= ds->force_read_until);
}

// Returns true if there was "progress" (lock was released temporarily).
static bool read_packet(struct demux_internal *in)
{
    in->eof = false;
    in->idle = true;

    if (!in->reading || in->blocked || demux_cancel_test(in->d_thread))
        return false;

    // Check if we need to read a new packet. We do this if all queues are below
    // the minimum, or if a stream explicitly needs new packets. Also includes
    // safe-guards against packet queue overflow.
    bool read_more = false, prefetch_more = false, refresh_more = false;
    uint64_t total_fw_bytes = 0;
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;
        if (ds->eager) {
            read_more |= !ds->reader_head;
        } else if (lazy_stream_force_read(ds)) {
            read_more = true;
        }
        refresh_more |= ds->refreshing;
        if (ds->eager && ds->queue->last_ts != MP_NOPTS_VALUE &&
            in->min_secs > 0 && ds->base_ts != MP_NOPTS_VALUE &&
            ds->queue->last_ts >= ds->base_ts)
            prefetch_more |= ds->queue->last_ts - ds->base_ts < in->min_secs;
        total_fw_bytes += get_foward_buffered_bytes(ds);
    }
    MP_TRACE(in, "bytes=%zd, read_more=%d prefetch_more=%d, refresh_more=%d\n",
             (size_t) total_fw_bytes, read_more, prefetch_more, refresh_more);

    if (total_fw_bytes >= in->max_bytes) {
        // if we hit the limit just by prefetching, simply stop prefetching
        if (!read_more)
            return false;
        if (!in->warned_queue_overflow) {
            in->warned_queue_overflow = true;
            MP_WARN(in, "Too many packets in the demuxer packet queues:\n");
            for (int n = 0; n < in->num_streams; n++) {
                struct demux_stream *ds = in->streams[n]->ds;
                if (ds->selected) {
                    size_t num_pkts = 0;
                    for (struct demux_packet *dp = ds->reader_head;
                         dp; dp = dp->next)
                        num_pkts++;
                    uint64_t fw_bytes = get_foward_buffered_bytes(ds);
                    MP_WARN(in, "  %s/%d: %zd packets, %zd bytes%s%s\n",
                            stream_type_name(ds->type), n,
                            num_pkts, (size_t)fw_bytes,
                            ds->eager ? "" : " (lazy)",
                            ds->refreshing ? " (refreshing)" : "");
                }
            }
        }
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            bool eof = !ds->reader_head;
            if (eof) {
                mark_stream_eof(ds);
            }
        }
        return false;
    }

    if (!read_more && !prefetch_more && !refresh_more)
        return false;

    if (in->after_seek_to_start) {
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            in->current_range->streams[n]->is_bof =
                ds->selected && !ds->refreshing;
        }
    }

    // Actually read a packet. Drop the lock while doing so, because waiting
    // for disk or network I/O can take time.
    in->idle = false;
    in->after_seek = false;
    in->after_seek_to_start = false;
    pthread_mutex_unlock(&in->lock);

    struct demuxer *demux = in->d_thread;

    bool eof = true;
    if (demux->desc->fill_buffer && !demux_cancel_test(demux))
        eof = demux->desc->fill_buffer(demux) <= 0;
    update_cache(in);

    pthread_mutex_lock(&in->lock);

    if (!in->seeking) {
        if (eof) {
            for (int n = 0; n < in->num_streams; n++)
                mark_stream_eof(in->streams[n]->ds);
            // If we had EOF previously, then don't wakeup (avoids wakeup loop)
            if (!in->last_eof) {
                if (in->wakeup_cb)
                    in->wakeup_cb(in->wakeup_cb_ctx);
                pthread_cond_signal(&in->wakeup);
                MP_VERBOSE(in, "EOF reached.\n");
            }
        }
        in->eof = in->last_eof = eof;
    }
    return true;
}

static void prune_old_packets(struct demux_internal *in)
{
    assert(in->current_range == in->ranges[in->num_ranges - 1]);

    // It's not clear what the ideal way to prune old packets is. For now, we
    // prune the oldest packet runs, as long as the total cache amount is too
    // big.
    size_t max_bytes = in->seekable_cache ? in->max_bytes_bw : 0;
    while (1) {
        uint64_t fw_bytes = 0;
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            fw_bytes += get_foward_buffered_bytes(ds);
        }
        if (in->total_bytes - fw_bytes <= max_bytes)
            break;

        // (Start from least recently used range.)
        struct demux_cached_range *range = in->ranges[0];
        double earliest_ts = MP_NOPTS_VALUE;
        struct demux_stream *earliest_stream = NULL;

        for (int n = 0; n < range->num_streams; n++) {
            struct demux_queue *queue = range->streams[n];
            struct demux_stream *ds = queue->ds;

            if (queue->head && queue->head != ds->reader_head) {
                struct demux_packet *dp = queue->head;
                double ts = dp->kf_seek_pts;
                // Note: in obscure cases, packets might have no timestamps set,
                // in which case we still need to prune _something_.
                bool prune_always =
                    !in->seekable_cache || ts == MP_NOPTS_VALUE || !dp->keyframe;
                if (prune_always || !earliest_stream || ts < earliest_ts) {
                    earliest_ts = ts;
                    earliest_stream = ds;
                    if (prune_always)
                        break;
                }
            }
        }

        assert(earliest_stream); // incorrect accounting of buffered sizes?
        struct demux_stream *ds = earliest_stream;
        struct demux_queue *queue = range->streams[ds->index];

        // Prune all packets until the next keyframe or reader_head. Keeping
        // those packets would not help with seeking at all, so we strictly
        // drop them.
        // In addition, we need to find the new possibly min. seek target,
        // which in the worst case could be inside the forward buffer. The fact
        // that many keyframe ranges without keyframes exist (audio packets)
        // makes this much harder.
        if (in->seekable_cache && !queue->next_prune_target) {
            // (Has to be _after_ queue->head to drop at least 1 packet.)
            struct demux_packet *prev = queue->head;
            if (queue->seek_start != MP_NOPTS_VALUE)
                queue->last_pruned = queue->seek_start;
            queue->seek_start = MP_NOPTS_VALUE;
            queue->keyframe_earliest = NULL;
            queue->next_prune_target = queue->tail; // (prune all if none found)
            while (prev->next) {
                struct demux_packet *dp = prev->next;
                // Note that the next back_pts might be above the lowest buffered
                // packet, but it will still be only viable lowest seek target.
                if (dp->keyframe && dp->kf_seek_pts != MP_NOPTS_VALUE) {
                    queue->seek_start = dp->kf_seek_pts;
                    queue->keyframe_earliest = dp;
                    queue->next_prune_target = prev;
                    break;
                }
                prev = prev->next;
            }
        }

        bool done = false;
        while (!done && queue->head && queue->head != ds->reader_head) {
            done = queue->next_prune_target == queue->head;
            remove_head_packet(queue);
        }
        update_seek_ranges(range);

        if (range != in->current_range && range->seek_start == MP_NOPTS_VALUE)
            free_empty_cached_ranges(in);
    }
}

static void execute_trackswitch(struct demux_internal *in)
{
    in->tracks_switched = false;

    bool any_selected = false;
    for (int n = 0; n < in->num_streams; n++)
        any_selected |= in->streams[n]->ds->selected;

    pthread_mutex_unlock(&in->lock);

    if (in->d_thread->desc->control)
        in->d_thread->desc->control(in->d_thread, DEMUXER_CTRL_SWITCHED_TRACKS, 0);

    stream_control(in->d_thread->stream, STREAM_CTRL_SET_READAHEAD,
                   &(int){any_selected});

    pthread_mutex_lock(&in->lock);
}

static void execute_seek(struct demux_internal *in)
{
    int flags = in->seek_flags;
    double pts = in->seek_pts;
    in->last_eof = in->eof = false;
    in->seeking = false;
    in->seeking_in_progress = pts;
    in->demux_ts = MP_NOPTS_VALUE;
    in->low_level_seeks += 1;
    in->after_seek = true;
    in->after_seek_to_start =
        !(flags & (SEEK_FORWARD | SEEK_FACTOR)) &&
        pts <= in->d_thread->start_time;

    for (int n = 0; n < in->num_streams; n++)
        in->streams[n]->ds->queue->last_pos_fixup = -1;

    pthread_mutex_unlock(&in->lock);

    MP_VERBOSE(in, "execute seek (to %f flags %d)\n", pts, flags);

    if (in->d_thread->desc->seek)
        in->d_thread->desc->seek(in->d_thread, pts, flags);

    MP_VERBOSE(in, "seek done\n");

    pthread_mutex_lock(&in->lock);

    in->seeking_in_progress = MP_NOPTS_VALUE;
}

// Make demuxing progress. Return whether progress was made.
static bool thread_work(struct demux_internal *in)
{
    if (in->run_fn) {
        in->run_fn(in->run_fn_arg);
        in->run_fn = NULL;
        pthread_cond_signal(&in->wakeup);
        return true;
    }
    if (in->tracks_switched) {
        execute_trackswitch(in);
        return true;
    }
    if (in->seeking) {
        execute_seek(in);
        return true;
    }
    if (!in->eof) {
        if (read_packet(in))
            return true; // read_packet unlocked, so recheck conditions
    }
    if (in->force_cache_update) {
        pthread_mutex_unlock(&in->lock);
        update_cache(in);
        pthread_mutex_lock(&in->lock);
        in->force_cache_update = false;
        return true;
    }
    return false;
}

static void *demux_thread(void *pctx)
{
    struct demux_internal *in = pctx;
    mpthread_set_name("demux");
    pthread_mutex_lock(&in->lock);
    while (!in->thread_terminate) {
        if (thread_work(in))
            continue;
        pthread_cond_signal(&in->wakeup);
        pthread_cond_wait(&in->wakeup, &in->lock);
    }
    pthread_mutex_unlock(&in->lock);
    return NULL;
}


static struct demux_packet *dequeue_packet(struct demux_stream *ds, double min_pts)
{
    if (ds->sh->attached_picture) {
        ds->eof = true;
        if (ds->attached_picture_added)
            return NULL;
        ds->attached_picture_added = true;
        struct demux_packet *pkt = demux_copy_packet(ds->sh->attached_picture);
        if (!pkt)
            abort();
        pkt->stream = ds->sh->index;
        return pkt;
    }
    ds->force_read_until = min_pts;
    if (!ds->reader_head || ds->in->blocked)
        return NULL;
    struct demux_packet *pkt = ds->reader_head;
    ds->reader_head = pkt->next;


    ds->last_ret_pos = pkt->pos;
    ds->last_ret_dts = pkt->dts;

    // The returned packet is mutated etc. and will be owned by the user.
    pkt = demux_copy_packet(pkt);
    if (!pkt)
        abort();
    pkt->next = NULL;

    double ts = PTS_OR_DEF(pkt->dts, pkt->pts);
    if (ts != MP_NOPTS_VALUE)
        ds->base_ts = ts;

    if (pkt->keyframe && ts != MP_NOPTS_VALUE) {
        // Update bitrate - only at keyframe points, because we use the
        // (possibly) reordered packet timestamps instead of realtime.
        double d = ts - ds->last_br_ts;
        if (ds->last_br_ts == MP_NOPTS_VALUE || d < 0) {
            ds->bitrate = -1;
            ds->last_br_ts = ts;
            ds->last_br_bytes = 0;
        } else if (d >= 0.5) { // a window of least 500ms for UI purposes
            ds->bitrate = ds->last_br_bytes / d;
            ds->last_br_ts = ts;
            ds->last_br_bytes = 0;
        }
    }
    ds->last_br_bytes += pkt->len;

    // This implies this function is actually called from "the" user thread.
    if (pkt->pos >= ds->in->d_user->filepos)
        ds->in->d_user->filepos = pkt->pos;

    pkt->pts = MP_ADD_PTS(pkt->pts, ds->in->ts_offset);
    pkt->dts = MP_ADD_PTS(pkt->dts, ds->in->ts_offset);

    if (pkt->segmented) {
        pkt->start = MP_ADD_PTS(pkt->start, ds->in->ts_offset);
        pkt->end = MP_ADD_PTS(pkt->end, ds->in->ts_offset);
    }

    // Apply timed metadata when packet is returned to user.
    // (The tags_init thing is a microopt. to not do refcounting for sane files.)
    struct mp_packet_tags *metadata = pkt->metadata;
    if (!metadata)
        metadata = ds->tags_init;
    if (metadata != ds->tags_reader) {
        mp_packet_tags_setref(&ds->tags_reader, metadata);
        ds->in->events |= DEMUX_EVENT_METADATA;
        if (ds->in->wakeup_cb)
            ds->in->wakeup_cb(ds->in->wakeup_cb_ctx);
    }

    prune_old_packets(ds->in);
    return pkt;
}

// Read a packet from the given stream. The returned packet belongs to the
// caller, who has to free it with talloc_free(). Might block. Returns NULL
// on EOF.
/**
This version returns a packet if one already exists, otherwise blocks until a new packet is ready.
Unlike the async version it doesn't trigger a readahead after the packet is ready.
*/
static struct demux_packet *demux_read_packet(struct sh_stream *sh, double min_pts, bool *blocked)
{
    struct demux_stream *ds = sh ? sh->ds : NULL;
    if (!ds)
        return NULL;
    struct demux_internal *in = ds->in;
    pthread_mutex_lock(&in->lock);
    struct demux_packet *pkt = dequeue_packet(ds, min_pts);
    if (pkt) {
        goto ret;
    }
    bool forced_read = lazy_stream_force_read(ds);
    if (ds->eager || forced_read) {
        const char *t = stream_type_name(ds->type);
        MP_DBG(in, "reading packet for %s\n", t);
        in->eof = false; // force retry
        ds->need_wakeup = true;
        // When ds->reader_head gets set, we have a new packet ready for us.
        while (ds->selected && !ds->reader_head && !in->blocked) {
            in->reading = true;
            // Note: the following code marks EOF if it can't continue
            // Even though we conceptually block, we _must_ use the demux thread
            // instead of calling thread_work() directly since thread_work can drop
            // the lock internally, which can cause race-conditions and bad behavior.
            if (in->threading) {
                MP_VERBOSE(in, "waiting for demux thread (%s)\n", t);
                pthread_cond_signal(&in->wakeup);
                pthread_cond_wait(&in->wakeup, &in->lock);
            } else {
                thread_work(in);
            }
            // As an extra precaution we also break if we no longer need to poll for lazy streams.
            // This guards against a scenario where we have no packet ready in a sparse stream
            // and keep looping endlessly.
            if (ds->eof || (!ds->eager && !lazy_stream_force_read(ds)))
                break;

        }
    }
    pkt = dequeue_packet(ds, min_pts);

ret:
    *blocked = in->blocked;
    pthread_cond_signal(&in->wakeup); // possibly read more
    pthread_mutex_unlock(&in->lock);
    return pkt;
}

// Poll the demuxer queue, and if there's a packet, return it. Otherwise, just
// make the demuxer thread read packets for this stream, and if there's at
// least one packet, call the wakeup callback.
// Unlike demux_read_packet(), this always enables readahead (except for
// interleaved subtitles).
// Returns:
//   < 0: EOF was reached, *out_pkt=NULL
//  == 0: no new packet yet, but maybe later, *out_pkt=NULL
//   > 0: new packet read, *out_pkt is set
// Note: when reading interleaved subtitles, the demuxer won't try to forcibly
// read ahead to get the next subtitle packet (as the next packet could be
// minutes away). In this situation, this function will just return -1.
// If called for an unselected track, this will likely always return 0.
// (In the future it may be changed to return cached data, but client responsibility
// to avoid spinning forever.)
int demux_read_packet_async(struct sh_stream *sh, struct demux_packet **out_pkt)
{
    return demux_read_packet_async_until(sh, MP_NOPTS_VALUE, out_pkt);
}


int demux_read_packet_async_until(struct sh_stream *sh, double min_pts, struct demux_packet **out_pkt)
{
    struct demux_stream *ds = sh ? sh->ds : NULL;
    int r = -1;
    *out_pkt = NULL;
    if (!ds)
        return r;
    if (!ds->in->threading) {
        return demux_read_packet_sync_until(sh, min_pts, out_pkt);
    }

    pthread_mutex_lock(&ds->in->lock);
    *out_pkt = dequeue_packet(ds, min_pts);
    // True (read-ahead) iff eager, or lazy stream & min_pts not satisfied
    // Basically we treat lazy-forced case as effectively eager.
    bool continue_reading = ds->eager || lazy_stream_force_read(ds);

    // If we expect a packet to be ready later (eager, or lazy-wait) ask user to check back later.
    // Otherwise we're done for now.
    //
    // Note that lazy-wait condition is based on timestamp of last (demuxed) packet in stream queue.
    // demux_add_packet takes a lock on in->lock, so lazy_stream_force_read cannot have changed 
    // within this critical section. I.e. it's not possible that in-between the time we
    // dequeued a packet and determined the return code, the demux thread added a new packet
    // to the queue causing stream_needs_wait to erroneously return false us us returning -1 instead 
    // of 0 (retry).
    r = *out_pkt ? 1 : (ds->eof  || !continue_reading ? -1 : 0);

    // Effectively eager stream should continue reading to make forward progress
    if (continue_reading) {
        ds->in->reading = true; // enable readahead
        ds->in->eof = false; // force retry
        pthread_cond_signal(&ds->in->wakeup); // possibly read more
    }
    ds->need_wakeup = r != 1;
    pthread_mutex_unlock(&ds->in->lock);
    
    return r;
}

int demux_read_packet_sync(struct sh_stream *sh, struct demux_packet **out_pkt)
{
    return demux_read_packet_sync_until(sh, MP_NOPTS_VALUE, out_pkt);
}

/**
  Unlike async version, this only ever returns 0 if demux was forcefully
  blocked via demux_block_reading.
*/
int demux_read_packet_sync_until(struct sh_stream *sh, double min_pts, struct demux_packet **out_pkt) {
    struct demux_stream *ds = sh ? sh->ds : NULL;
    int r = -1;
    *out_pkt = NULL;
    if (!ds)
        return r;

    bool blocked = false;
    *out_pkt = demux_read_packet(sh, min_pts, &blocked);
    r = *out_pkt ? 1 : (blocked ? 0 : -1);
    ds->need_wakeup = r != 1;
    return r;
}

// Return whether a packet is queued. Never blocks, never forces any reads.
bool demux_has_packet(struct sh_stream *sh)
{
    bool has_packet = false;
    if (sh) {
        pthread_mutex_lock(&sh->ds->in->lock);
        has_packet = sh->ds->reader_head;
        pthread_mutex_unlock(&sh->ds->in->lock);
    }
    return has_packet;
}

// Read and return any packet we find. NULL means EOF.
// Only works if demuxer is created without threading enabled,
// usually used for slave demxuers.
struct demux_packet *demux_read_any_packet(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(!in->threading); // doesn't work with threading
    bool read_more = true;
    while (read_more && !in->blocked) {
        for (int n = 0; n < in->num_streams; n++) {
            in->reading = true; // force read_packet() to read
            struct demux_packet *pkt = dequeue_packet(in->streams[n]->ds, MP_NOPTS_VALUE);
            if (pkt)
                return pkt;
        }
        // retry after calling this
        pthread_mutex_lock(&in->lock); // lock only because thread_work unlocks
        read_more = thread_work(in);
        read_more &= !in->eof;
        pthread_mutex_unlock(&in->lock);
    }
    return NULL;
}

void demuxer_help(struct mp_log *log)
{
    int i;

    mp_info(log, "Available demuxers:\n");
    mp_info(log, " demuxer:   info:\n");
    for (i = 0; demuxer_list[i]; i++) {
        mp_info(log, "%10s  %s\n",
                demuxer_list[i]->name, demuxer_list[i]->desc);
    }
}

static const char *d_level(enum demux_check level)
{
    switch (level) {
    case DEMUX_CHECK_FORCE:  return "force";
    case DEMUX_CHECK_UNSAFE: return "unsafe";
    case DEMUX_CHECK_REQUEST:return "request";
    case DEMUX_CHECK_NORMAL: return "normal";
    }
    abort();
}

static int decode_float(char *str, float *out)
{
    char *rest;
    float dec_val;

    dec_val = strtod(str, &rest);
    if (!rest || (rest == str) || !isfinite(dec_val))
        return -1;

    *out = dec_val;
    return 0;
}

static int decode_gain(struct mp_log *log, struct mp_tags *tags,
                       const char *tag, float *out)
{
    char *tag_val = NULL;
    float dec_val;

    tag_val = mp_tags_get_str(tags, tag);
    if (!tag_val)
        return -1;

    if (decode_float(tag_val, &dec_val) < 0) {
        mp_msg(log, MSGL_ERR, "Invalid replaygain value\n");
        return -1;
    }

    *out = dec_val;
    return 0;
}

static int decode_peak(struct mp_log *log, struct mp_tags *tags,
                       const char *tag, float *out)
{
    char *tag_val = NULL;
    float dec_val;

    *out = 1.0;

    tag_val = mp_tags_get_str(tags, tag);
    if (!tag_val)
        return 0;

    if (decode_float(tag_val, &dec_val) < 0 || dec_val <= 0.0)
        return -1;

    *out = dec_val;
    return 0;
}

static struct replaygain_data *decode_rgain(struct mp_log *log,
                                            struct mp_tags *tags)
{
    struct replaygain_data rg = {0};

    if (decode_gain(log, tags, "REPLAYGAIN_TRACK_GAIN", &rg.track_gain) >= 0 &&
        decode_peak(log, tags, "REPLAYGAIN_TRACK_PEAK", &rg.track_peak) >= 0)
    {
        if (decode_gain(log, tags, "REPLAYGAIN_ALBUM_GAIN", &rg.album_gain) < 0 ||
            decode_peak(log, tags, "REPLAYGAIN_ALBUM_PEAK", &rg.album_peak) < 0)
        {
            rg.album_gain = rg.track_gain;
            rg.album_peak = rg.track_peak;
        }
        return talloc_dup(NULL, &rg);
    }

    if (decode_gain(log, tags, "REPLAYGAIN_GAIN", &rg.track_gain) >= 0 &&
        decode_peak(log, tags, "REPLAYGAIN_PEAK", &rg.track_peak) >= 0)
    {
        rg.album_gain = rg.track_gain;
        rg.album_peak = rg.track_peak;
        return talloc_dup(NULL, &rg);
    }

    return NULL;
}

static void demux_update_replaygain(demuxer_t *demuxer)
{
    struct demux_internal *in = demuxer->in;
    for (int n = 0; n < in->num_streams; n++) {
        struct sh_stream *sh = in->streams[n];
        if (sh->type == STREAM_AUDIO && !sh->codec->replaygain_data) {
            struct replaygain_data *rg = decode_rgain(demuxer->log, sh->tags);
            if (!rg)
                rg = decode_rgain(demuxer->log, demuxer->metadata);
            if (rg)
                sh->codec->replaygain_data = talloc_steal(in, rg);
        }
    }
}

// Copy some fields from src to dst (for initialization).
static void demux_copy(struct demuxer *dst, struct demuxer *src)
{
    // Note that we do as shallow copies as possible. We expect the data
    // that is not-copied (only referenced) to be immutable.
    // This implies e.g. that no chapters are added after initialization.
    dst->chapters = src->chapters;
    dst->num_chapters = src->num_chapters;
    dst->editions = src->editions;
    dst->num_editions = src->num_editions;
    dst->edition = src->edition;
    dst->attachments = src->attachments;
    dst->num_attachments = src->num_attachments;
    dst->matroska_data = src->matroska_data;
    dst->playlist = src->playlist;
    dst->seekable = src->seekable;
    dst->partially_seekable = src->partially_seekable;
    dst->filetype = src->filetype;
    dst->ts_resets_possible = src->ts_resets_possible;
    dst->fully_read = src->fully_read;
    dst->start_time = src->start_time;
    dst->duration = src->duration;
    dst->is_network = src->is_network;
    dst->is_streaming = src->is_streaming;
    dst->priv = src->priv;
    dst->metadata = mp_tags_dup(dst, src->metadata);
}

// This is called by demuxer implementations if demuxer->metadata changed.
// (It will be propagated to the user as timed metadata.)
void demux_metadata_changed(demuxer_t *demuxer)
{
    assert(demuxer == demuxer->in->d_thread); // call from demuxer impl. only
    struct demux_internal *in = demuxer->in;

    pthread_mutex_lock(&in->lock);

    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;
        ds_modify_demux_tags(ds);
        mp_tags_replace(ds->tags_demux->demux, demuxer->metadata);
    }

    pthread_mutex_unlock(&in->lock);
}

// Called locked, with user demuxer.
static void update_final_metadata(demuxer_t *demuxer)
{
    assert(demuxer == demuxer->in->d_user);
    struct demux_internal *in = demuxer->in;

    int num_streams = MPMIN(in->num_streams, demuxer->num_update_stream_tags);
    for (int n = 0; n < num_streams; n++) {
        struct sh_stream *sh = in->streams[n];
        // (replace them even if unnecessary, simpler and doesn't hurt)
        if (sh->ds->tags_reader)
            mp_tags_replace(sh->tags, sh->ds->tags_reader->sh);
    }

    struct mp_packet_tags *tags =
        in->master_stream ? in->master_stream->tags_reader : NULL;

    if (tags)
        mp_tags_replace(demuxer->metadata, tags->demux);

    // Often for useful audio-only files, which have metadata in the audio track
    // metadata instead of the main metadata, but can also have cover art
    // metadata (which libavformat likes to treat as video streams).
    int astreams = 0;
    int astream_id = -1;
    int vstreams = 0;
    for (int n = 0; n < in->num_streams; n++) {
        struct sh_stream *sh = in->streams[n];
        if (sh->type == STREAM_VIDEO && !sh->attached_picture)
            vstreams += 1;
        if (sh->type == STREAM_AUDIO) {
            astreams += 1;
            astream_id = n;
        }
    }
    if (vstreams == 0 && astreams == 1)
        mp_tags_merge(demuxer->metadata, in->streams[astream_id]->tags);

    if (tags)
        mp_tags_merge(demuxer->metadata, tags->stream);
}

// Called by the user thread (i.e. player) to update metadata and other things
// from the demuxer thread.
void demux_update(demuxer_t *demuxer)
{
    assert(demuxer == demuxer->in->d_user);
    struct demux_internal *in = demuxer->in;

    if (!in->threading)
        update_cache(in);

    pthread_mutex_lock(&in->lock);
    demuxer->events |= in->events;
    in->events = 0;
    if (demuxer->events & DEMUX_EVENT_METADATA)
        update_final_metadata(demuxer);
    if (demuxer->events & (DEMUX_EVENT_METADATA | DEMUX_EVENT_STREAMS))
        demux_update_replaygain(demuxer);
    if (demuxer->events & DEMUX_EVENT_DURATION)
        demuxer->duration = in->duration;
    pthread_mutex_unlock(&in->lock);
}

static void demux_init_cache(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    struct stream *stream = demuxer->stream;

    char *base = NULL;
    stream_control(stream, STREAM_CTRL_GET_BASE_FILENAME, &base);
    in->stream_base_filename = talloc_steal(demuxer, base);
}

static void demux_init_cuesheet(struct demuxer *demuxer)
{
    char *cue = mp_tags_get_str(demuxer->metadata, "cuesheet");
    if (cue && !demuxer->num_chapters) {
        struct cue_file *f = mp_parse_cue(bstrof0(cue));
        if (f) {
            if (mp_check_embedded_cue(f) < 0) {
                MP_WARN(demuxer, "Embedded cue sheet references more than one file. "
                        "Ignoring it.\n");
            } else {
                for (int n = 0; n < f->num_tracks; n++) {
                    struct cue_track *t = &f->tracks[n];
                    int idx = demuxer_add_chapter(demuxer, "", t->start, -1);
                    mp_tags_merge(demuxer->chapters[idx].metadata, t->tags);
                }
            }
        }
        talloc_free(f);
    }
}

static void demux_maybe_replace_stream(struct demuxer *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(!in->threading && demuxer == in->d_user);

    if (demuxer->fully_read) {
        MP_VERBOSE(demuxer, "assuming demuxer read all data; closing stream\n");
        free_stream(demuxer->stream);
        demuxer->stream = open_memory_stream(NULL, 0); // dummy
        in->d_thread->stream = demuxer->stream;

        if (demuxer->desc->control)
            demuxer->desc->control(in->d_thread, DEMUXER_CTRL_REPLACE_STREAM, NULL);
    }
}

static void demux_init_ccs(struct demuxer *demuxer, struct demux_opts *opts)
{
    struct demux_internal *in = demuxer->in;
    if (!opts->create_ccs)
        return;
    pthread_mutex_lock(&in->lock);
    for (int n = 0; n < in->num_streams; n++) {
        struct sh_stream *sh = in->streams[n];
        if (sh->type == STREAM_VIDEO)
            demuxer_get_cc_track_locked(sh);
    }
    pthread_mutex_unlock(&in->lock);
}

// Each stream contains a copy of the global demuxer metadata, but this might
// be outdated if a stream gets added and then metadata does get set during
// early init.
static void fixup_metadata(struct demux_internal *in)
{
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;
        mp_packet_tags_make_writable(&ds->tags_init);
        mp_tags_replace(ds->tags_init->demux, in->d_thread->metadata);
        mp_packet_tags_setref(&ds->tags_reader, ds->tags_init);
    }
}

static struct demuxer *open_given_type(struct mpv_global *global,
                                       struct mp_log *log,
                                       const struct demuxer_desc *desc,
                                       struct stream *stream,
                                       struct demuxer_params *params,
                                       enum demux_check check)
{
    if (mp_cancel_test(stream->cancel))
        return NULL;

    struct demuxer *demuxer = talloc_ptrtype(NULL, demuxer);
    struct demux_opts *opts = mp_get_config_group(demuxer, global, &demux_conf);
    *demuxer = (struct demuxer) {
        .desc = desc,
        .stream = stream,
        .seekable = stream->seekable,
        .filepos = -1,
        .global = global,
        .log = mp_log_new(demuxer, log, desc->name),
        .glog = log,
        .filename = talloc_strdup(demuxer, stream->url),
        .is_network = stream->is_network,
        .is_streaming = stream->streaming,
        .access_references = opts->access_references,
        .events = DEMUX_EVENT_ALL,
        .duration = -1,
    };
    demuxer->seekable = stream->seekable;
    if (demuxer->stream->underlying && !demuxer->stream->underlying->seekable)
        demuxer->seekable = false;

    struct demux_internal *in = demuxer->in = talloc_ptrtype(demuxer, in);
    *in = (struct demux_internal){
        .log = demuxer->log,
        .d_thread = talloc(demuxer, struct demuxer),
        .d_user = demuxer,
        .min_secs = opts->min_secs,
        .max_bytes = opts->max_bytes,
        .max_bytes_bw = opts->max_bytes_bw,
        .after_seek = true, // (assumed identical to initial demuxer state)
        .after_seek_to_start = true,
        .highest_av_pts = MP_NOPTS_VALUE,
        .seeking_in_progress = MP_NOPTS_VALUE,
        .demux_ts = MP_NOPTS_VALUE,
    };
    pthread_mutex_init(&in->lock, NULL);
    pthread_cond_init(&in->wakeup, NULL);

    in->current_range = talloc_ptrtype(in, in->current_range);
    *in->current_range = (struct demux_cached_range){
        .seek_start = MP_NOPTS_VALUE,
        .seek_end = MP_NOPTS_VALUE,
    };
    MP_TARRAY_APPEND(in, in->ranges, in->num_ranges, in->current_range);

    *in->d_thread = *demuxer;

    in->d_thread->metadata = talloc_zero(in->d_thread, struct mp_tags);

    mp_dbg(log, "Trying demuxer: %s (force-level: %s)\n",
           desc->name, d_level(check));

    // not for DVD/BD/DVB in particular
    if (stream->seekable && (!params || !params->timeline))
        stream_seek(stream, 0);

    // Peek this much data to avoid that stream_read() run by some demuxers
    // will flush previous peeked data.
    stream_peek(stream, STREAM_BUFFER_SIZE);

    in->d_thread->params = params; // temporary during open()
    int ret = demuxer->desc->open(in->d_thread, check);
    if (ret >= 0) {
        in->d_thread->params = NULL;
        if (in->d_thread->filetype)
            mp_verbose(log, "Detected file format: %s (%s)\n",
                       in->d_thread->filetype, desc->desc);
        else
            mp_verbose(log, "Detected file format: %s\n", desc->desc);
        if (!in->d_thread->seekable)
            mp_verbose(log, "Stream is not seekable.\n");
        if (!in->d_thread->seekable && opts->force_seekable) {
            mp_warn(log, "Not seekable, but enabling seeking on user request.\n");
            in->d_thread->seekable = true;
            in->d_thread->partially_seekable = true;
        }
        demux_init_cuesheet(in->d_thread);
        demux_init_cache(demuxer);
        demux_init_ccs(demuxer, opts);
        demux_copy(in->d_user, in->d_thread);
        in->duration = in->d_thread->duration;
        demuxer_sort_chapters(demuxer);
        fixup_metadata(in);
        in->events = DEMUX_EVENT_ALL;
        demux_update(demuxer);
        stream_control(demuxer->stream, STREAM_CTRL_SET_READAHEAD,
                       &(int){params ? params->initial_readahead : false});
        
        int use_demux_cache = opts->demux_cache;
        if (use_demux_cache < 0) {
            // This allows us to use demux cache on auto even when stream cache is disabled.
            use_demux_cache = demuxer->is_streaming || demuxer->is_network || stream->caching;
        }
        // Never use demux cache for "fully read" streams. These are effectively
        // already cached by the underlying demuxer impl (currently only libavformat)
        use_demux_cache = use_demux_cache && !demuxer->fully_read;
        int seekable = opts->seekable_cache;
        if (use_demux_cache) {
            in->min_secs = MPMAX(in->min_secs, opts->min_secs_cache);
            if (seekable < 0)
                seekable = 1;
        }
        in->seekable_cache = seekable == 1;
        if (!(params && params->disable_timeline)) {
            struct timeline *tl = timeline_load(global, log, demuxer);
            if (tl) {
                struct demuxer_params params2 = {0};
                params2.timeline = tl;
                struct demuxer *sub =
                    open_given_type(global, log, &demuxer_desc_timeline, stream,
                                    &params2, DEMUX_CHECK_FORCE);
                if (sub) {
                    demuxer = sub;
                } else {
                    timeline_destroy(tl);
                }
            }
        }
        return demuxer;
    }

    free_demuxer(demuxer);
    return NULL;
}

static const int d_normal[]  = {DEMUX_CHECK_NORMAL, DEMUX_CHECK_UNSAFE, -1};
static const int d_request[] = {DEMUX_CHECK_REQUEST, -1};
static const int d_force[]   = {DEMUX_CHECK_FORCE, -1};

// params can be NULL
struct demuxer *demux_open(struct stream *stream, struct demuxer_params *params,
                           struct mpv_global *global)
{
    const int *check_levels = d_normal;
    const struct demuxer_desc *check_desc = NULL;
    struct mp_log *log = mp_log_new(NULL, global->log, "!demux");
    struct demuxer *demuxer = NULL;
    char *force_format = params ? params->force_format : NULL;

    if (!force_format)
        force_format = stream->demuxer;

    if (force_format && force_format[0]) {
        check_levels = d_request;
        if (force_format[0] == '+') {
            force_format += 1;
            check_levels = d_force;
        }
        for (int n = 0; demuxer_list[n]; n++) {
            if (strcmp(demuxer_list[n]->name, force_format) == 0)
                check_desc = demuxer_list[n];
        }
        if (!check_desc) {
            mp_err(log, "Demuxer %s does not exist.\n", force_format);
            goto done;
        }
    }

    // Test demuxers from first to last, one pass for each check_levels[] entry
    for (int pass = 0; check_levels[pass] != -1; pass++) {
        enum demux_check level = check_levels[pass];
        mp_verbose(log, "Trying demuxers for level=%s.\n", d_level(level));
        for (int n = 0; demuxer_list[n]; n++) {
            const struct demuxer_desc *desc = demuxer_list[n];
            if (!check_desc || desc == check_desc) {
                demuxer = open_given_type(global, log, desc, stream, params, level);
                if (demuxer) {
                    talloc_steal(demuxer, log);
                    log = NULL;
                    goto done;
                }
            }
        }
    }

done:
    talloc_free(log);
    return demuxer;
}

// Convenience function: open the stream, enable the cache (according to params
// and global opts.), open the demuxer.
// (use free_demuxer_and_stream() to free the underlying stream too)
// Also for some reason may close the opened stream if it's not needed.
struct demuxer *demux_open_url(const char *url,
                                struct demuxer_params *params,
                                struct mp_cancel *cancel,
                                struct mpv_global *global)
{
    struct demuxer_params dummy = {0};
    if (!params)
        params = &dummy;
    struct stream *s = stream_create(url, STREAM_READ | params->stream_flags,
                                     cancel, global);
    if (!s)
        return NULL;
    if (!params->disable_cache)
        stream_enable_cache_defaults(&s);
    struct demuxer *d = demux_open(s, params, global);
    if (d) {
        demux_maybe_replace_stream(d);
    } else {
        params->demuxer_failed = true;
        free_stream(s);
    }
    return d;
}

// called locked, from user thread only
static void clear_reader_state(struct demux_internal *in)
{
    for (int n = 0; n < in->num_streams; n++)
        ds_clear_reader_state(in->streams[n]->ds);
    in->warned_queue_overflow = false;
    in->d_user->filepos = -1; // implicitly synchronized
    in->blocked = false;
}

// clear the packet queues
void demux_flush(demuxer_t *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    pthread_mutex_lock(&demuxer->in->lock);
    clear_reader_state(in);
    for (int n = 0; n < in->num_ranges; n++)
        clear_cached_range(in, in->ranges[n]);
    free_empty_cached_ranges(in);
    pthread_mutex_unlock(&demuxer->in->lock);
}

// Does some (but not all) things for switching to another range.
static void switch_current_range(struct demux_internal *in,
                                 struct demux_cached_range *range)
{
    struct demux_cached_range *old = in->current_range;
    assert(old != range);

    set_current_range(in, range);

    // Remove packets which can't be used when seeking back to the range.
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_queue *queue = old->streams[n];

        // Remove all packets from head up until including next_prune_target.
        while (queue->next_prune_target)
            remove_head_packet(queue);
    }

    // Exclude weird corner cases that break resuming.
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;
        // This is needed to resume or join the range at all.
        if (ds->selected && !(ds->global_correct_dts || ds->global_correct_pos)) {
            MP_VERBOSE(in, "discarding old range, due to stream %d: "
                       "correct_dts=%d correct_pos=%d\n", n,
                       ds->global_correct_dts, ds->global_correct_pos);
            clear_cached_range(in, old);
            break;
        }
    }

    // Set up reading from new range (as well as writing to it).
    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;

        ds->queue = range->streams[n];
        ds->refreshing = false;
        ds->eof = false;
    }

    // No point in keeping any junk (especially if old current_range is empty).
    free_empty_cached_ranges(in);
}

// Must be called locked, and only if cache is seekable.
static struct demux_packet *find_cache_seek_target(struct demux_queue *queue,
                                             double pts, int flags)
{
    assert(queue->ds->in->seekable_cache);

    struct demux_packet *start = queue->keyframe_earliest;
    for (int n = 0; n < queue->num_index; n++) {
        if (queue->index[n]->kf_seek_pts > pts)
            break;
        start = queue->index[n];
    }

    struct demux_packet *target = NULL;
    struct demux_packet *search_end = queue->keyframe_latest ? queue->keyframe_latest->next : NULL;

    for (struct demux_packet *dp = start; dp != search_end; dp = dp->next) {
        double range_pts = dp->kf_seek_pts;
        if (!dp->keyframe || range_pts == MP_NOPTS_VALUE)
            continue;

        if (flags & SEEK_FORWARD) {
            // Stop on the first keyframe whose seek_pts is >= pts.
            if (target)
                break;
            if (range_pts < pts)
                continue;
        } else {
            // Stop before the first keyframe whose seek_pts is > pts.
            // (I.e. return the highest keyframe whose seek_pts <= pts)
            // This still returns a kf with seek_pts > pts if there's no better one.
            // (Almost always the returned keyframe will in fact have seek_pts <= pts since
            // we usually only call this method if target pts >= keyframe_earliest seek_pts.
            // But in the case where range overlaps with BOF, the keyframe we return may be
            // later than what was requested.)
            if (target && range_pts > pts)
                break;
        }

        target = dp;
    }

    // Usually, the last seen keyframe (keyframe_latest) has kf_seek_pts unset
    // (because it needs to see all packets until the next keyframe or EOF in
    // order to determine the minimum PTS the range provides). If the pts is
    // within seek range, but the second-last keyframe is before the seek
    // target, above search will return NULL, even though we should return
    // keyframe_latest.
    //
    // This is only correct in the case when we've actually seen 2 keyframes,
    // which we check by making sure seek end is valid (and just for sanity
    // that the target PTS is also within the seek range, since while usually
    // we should never call this function if the range isn't valid for the pts,
    // in the case where the range overlaps with BOF or EOF that's not necessarily the case).
    // We also need queue->keyframe_latest to actually be defined, (since we may
    // call this even though range doesn't contain any packets).
    if (!target && (flags & SEEK_FORWARD) && queue->keyframe_latest &&
        queue->keyframe_latest->kf_seek_pts == MP_NOPTS_VALUE &&
        pts <= queue->seek_end)
    {
        target = queue->keyframe_latest;
    }

    return target;
}

// must be called locked
static struct demux_cached_range *find_cache_seek_range(struct demux_internal *in,
                                                         double pts, int flags)
{
    // Note about queued low level seeks: in->seeking can be true here, and it
    // might come from a previous resume seek to the current range. If we end
    // up seeking into the current range (i.e. just changing time offset), the
    // seek needs to continue. Otherwise, we override the queued seek anyway.
    if ((flags & SEEK_FACTOR) || !in->seekable_cache)
        return NULL;

    for (int n = 0; n < in->num_ranges; n++) {
        struct demux_cached_range *r = in->ranges[n];
        if (r->seek_start != MP_NOPTS_VALUE) {
            MP_VERBOSE(in, "cached range %d: %f <-> %f (bof=%d, eof=%d)\n",
                       n, r->seek_start, r->seek_end, r->is_bof, r->is_eof);

            if ((pts >= r->seek_start || r->is_bof) &&
                (pts <= r->seek_end || r->is_eof))
            {
                MP_VERBOSE(in, "...using this range for in-cache seek.\n");
                return r;
            }
        }
    }

    return NULL;
}

// must be called locked
// range must be non-NULL and from find_cache_seek_range() using the same pts
// and flags, before any other changes to the cached state
static void execute_cache_seek(struct demux_internal *in,
                               struct demux_cached_range *range,
                               double pts, int flags)
{
    // Adjust the seek target to the found video key frames. Otherwise the
    // video will undershoot the seek target, while audio will be closer to it.
    // The player frontend will play the additional video without audio, so
    // you get silent audio for the amount of "undershoot". Adjusting the seek
    // target will make the audio seek to the video target or before.
    // (If hr-seeks are used, it's better to skip this, as it would only mean
    // that more audio data than necessary would have to be decoded.)
    if (!(flags & SEEK_HR)) {
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            struct demux_queue *queue = range->streams[n];
            if (ds->selected && ds->type == STREAM_VIDEO) {
                struct demux_packet *target = find_cache_seek_target(queue, pts, flags);
                if (target) {
                    double target_pts = target->kf_seek_pts;
                    if (target_pts != MP_NOPTS_VALUE) {
                        MP_VERBOSE(in, "adjust seek target %f -> %f\n",
                                   pts, target_pts);
                        // (We assume the find_cache_seek_target() will return the
                        // same target for the video stream.)
                        pts = target_pts;
                        flags &= ~SEEK_FORWARD;
                    }
                }
                break;
            }
        }
    }

    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;
        struct demux_queue *queue = range->streams[n];

        struct demux_packet *target = find_cache_seek_target(queue, pts, flags);
        if (!(flags & SEEK_FORWARD) && ds->selected && ds->eager && !target) {
            // A backward seek should always return _some_ packet, since we only ever
            // do a cached seek if range start is defined which means we have
            // an earliest keyframe pts, and that keyframe should always satisfy a
            // seek-backward request.
            MP_ERR(in, "Cached seek backward from pts %f failed. Flags %d\n", pts, flags);
        }
        ds->reader_head = target;
        ds->skip_to_keyframe = !target;
        if (ds->reader_head)
            ds->base_ts = PTS_OR_DEF(ds->reader_head->pts, ds->reader_head->dts);

        MP_VERBOSE(in, "seeking stream %d (%s) to ",
                   n, stream_type_name(ds->type));

        if (target) {
            MP_VERBOSE(in, "packet %f/%f\n", target->pts, target->dts);
        } else {
            MP_VERBOSE(in, "nothing\n");
        }
    }

    // If we seek to another range, we want to seek the low level demuxer to
    // there as well, because reader and demuxer queue must be the same.
    if (in->current_range != range) {
        switch_current_range(in, range);

        in->seeking = true;
        in->seek_flags = SEEK_HR;
        in->seek_pts = range->seek_end - 1.0;

        // When new packets are being appended, they could overlap with the old
        // range due to demuxer seek imprecisions, or because the queue contains
        // packets past the seek target but before the next seek target. Don't
        // append them twice, instead skip them until new packets are found.
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;

            ds->refreshing = ds->selected;
        }

        MP_VERBOSE(in, "resuming demuxer to end of cached range\n");
    }
}

// Create a new blank cache range, and backup the old one. If the seekable
// demuxer cache is disabled, merely reset the current range to a blank state.
static void switch_to_fresh_cache_range(struct demux_internal *in)
{
    if (!in->seekable_cache) {
        clear_cached_range(in, in->current_range);
        return;
    }

    struct demux_cached_range *range = talloc_ptrtype(in, range);
    *range = (struct demux_cached_range){
        .seek_start = MP_NOPTS_VALUE,
        .seek_end = MP_NOPTS_VALUE,
    };
    MP_TARRAY_APPEND(in, in->ranges, in->num_ranges, range);
    add_missing_streams(in, range);

    switch_current_range(in, range);
}

int demux_seek(demuxer_t *demuxer, double seek_pts, int flags) {
    return demux_seek_with_offset(demuxer, seek_pts, 0, flags);
}

int demux_seek_with_offset(demuxer_t *demuxer, double seek_pts, double seek_offset, int flags)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);
    int res = 0;

    seek_pts += seek_offset;

    if ((flags & SEEK_HR) && (flags & SEEK_STRICT) && !(flags & SEEK_FORWARD)) {
        // Always try to compensate for possibly bad demuxers in "special"
        // situations where we need more robustness from the hr-seek code, even
        // if the user doesn't use --hr-seek-demuxer-offset.
        // The value is arbitrary, but should be "good enough" in most situations.
        if (-seek_offset < 0.5) {
            seek_pts = seek_pts - seek_offset - 0.5;
        }
    }

    pthread_mutex_lock(&in->lock);

    if (seek_pts == MP_NOPTS_VALUE)
        goto done;

    MP_VERBOSE(in, "queuing seek to %f%s\n", seek_pts,
               in->seeking ? " (cascade)" : "");

    if (!(flags & SEEK_FACTOR))
        seek_pts = MP_ADD_PTS(seek_pts, -in->ts_offset);

    bool require_cache = flags & SEEK_CACHED;
    flags &= ~(unsigned)SEEK_CACHED;

    struct demux_cached_range *cache_target =
        find_cache_seek_range(in, seek_pts, flags);

    if (!cache_target) {
        if (require_cache) {
            MP_VERBOSE(demuxer, "Cached seek not possible.\n");
            goto done;
        }
        if (!demuxer->seekable) {
            MP_WARN(demuxer, "Cannot seek in this file.\n");
            goto done;
        }
    }

    clear_reader_state(in);

    in->eof = false;
    in->idle = true;
    in->reading = false;

    if (cache_target) {
        execute_cache_seek(in, cache_target, seek_pts, flags);
    } else {
        switch_to_fresh_cache_range(in);

        in->seeking = true;
        in->seek_flags = flags;
        in->seek_pts = seek_pts;
    }

    for (int n = 0; n < in->num_streams; n++)
        wakeup_ds(in->streams[n]->ds);

    if (!in->threading && in->seeking)
        execute_seek(in);

    res = 1;

done:
    pthread_cond_signal(&in->wakeup);
    pthread_mutex_unlock(&in->lock);
    return res;
}

struct sh_stream *demuxer_stream_by_demuxer_id(struct demuxer *d,
                                               enum stream_type t, int id)
{
    int num = demux_get_num_stream(d);
    for (int n = 0; n < num; n++) {
        struct sh_stream *s = demux_get_stream(d, n);
        if (s->type == t && s->demuxer_id == id)
            return s;
    }
    return NULL;
}

// An obscure mechanism to get stream switching to be executed "faster" (as
// perceived by the user), by making the stream return packets from the
// current position
// On a switch, it seeks back, and then grabs all packets that were
// "missing" from the packet queue of the newly selected stream.
static void initiate_refresh_seek(struct demux_internal *in,
                                  struct demux_stream *stream,
                                  double start_ts)
{
    struct demuxer *demux = in->d_thread;
    bool seekable = demux->desc->seek && demux->seekable &&
                    !demux->partially_seekable;
    bool normal_seek = true;
    bool refresh_possible = true;
    
    // Prefetch subtitles on track switch a bit more.
    if (stream->type == STREAM_SUB) {
        start_ts -= 10.0;
    }

    for (int n = 0; n < in->num_streams; n++) {
        struct demux_stream *ds = in->streams[n]->ds;

        if (!ds->selected)
            continue;

        if (ds->type == STREAM_VIDEO || ds->type == STREAM_AUDIO)
            start_ts = MP_PTS_MIN(start_ts, ds->base_ts);

        // If there were no other streams selected, we can use a normal seek.
        normal_seek &= stream == ds;

        refresh_possible &= ds->queue->correct_dts || ds->queue->correct_pos;
    }

    if (start_ts == MP_NOPTS_VALUE || !seekable)
        return;

    if (!normal_seek) {
        if (!refresh_possible) {
            MP_VERBOSE(in, "can't issue refresh seek\n");
            return;
        }

        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;

            bool correct_pos = ds->queue->correct_pos;
            bool correct_dts = ds->queue->correct_dts;

            // We need to re-read all packets anyway, so discard the buffered
            // data. (In theory, we could keep the packets, and be able to use
            // it for seeking if partially read streams are deselected again,
            // but this causes other problems like queue overflows when
            // selecting a new stream.)
            ds_clear_reader_queue_state(ds);
            clear_queue(ds->queue);

            // Streams which didn't have any packets yet will return all packets,
            // other streams return packets only starting from the last position.
            if (ds->selected && (ds->last_ret_pos != -1 ||
                                 ds->last_ret_dts != MP_NOPTS_VALUE))
            {
                ds->refreshing = true;
                ds->queue->correct_dts = correct_dts;
                ds->queue->correct_pos = correct_pos;
                ds->queue->last_pos = ds->last_ret_pos;
                ds->queue->last_dts = ds->last_ret_dts;
            }

            update_seek_ranges(in->current_range);
        }

        start_ts -= 1.0; // small offset to get correct overlap
    }

    MP_VERBOSE(in, "refresh seek to %f\n", start_ts);
    in->seeking = true;
    in->seek_flags = SEEK_HR;
    in->seek_pts = start_ts;
}

// Set whether the given stream should return packets.
// ref_pts is used only if the stream is enabled. Then it serves as approximate
// start pts for this stream (in the worst case it is ignored).
void demuxer_select_track(struct demuxer *demuxer, struct sh_stream *stream,
                          double ref_pts, bool selected)
{
    struct demux_internal *in = demuxer->in;
    struct demux_stream *ds = stream->ds;
    pthread_mutex_lock(&in->lock);
    // don't flush buffers if stream is already selected / unselected
    if (ds->selected != selected) {
        MP_VERBOSE(in, "%sselect track %d\n", selected ? "" : "de", stream->index);
        ds->selected = selected;
        update_stream_selection_state(in, ds);
        in->tracks_switched = true;
        if (ds->selected && !in->after_seek)
            initiate_refresh_seek(in, ds, MP_ADD_PTS(ref_pts, -in->ts_offset));
        if (in->threading) {
            pthread_cond_signal(&in->wakeup);
        } else {
            execute_trackswitch(in);
        }
    }
    pthread_mutex_unlock(&in->lock);
}

void demux_set_stream_autoselect(struct demuxer *demuxer, bool autoselect)
{
    assert(!demuxer->in->threading); // laziness
    demuxer->in->autoselect = autoselect;
}

// This is for demuxer implementations only. demuxer_select_track() sets the
// logical state, while this function returns the actual state (in case the
// demuxer attempts to cache even unselected packets for track switching - this
// will potentially be done in the future).
bool demux_stream_is_selected(struct sh_stream *stream)
{
    if (!stream)
        return false;
    bool r = false;
    pthread_mutex_lock(&stream->ds->in->lock);
    r = stream->ds->selected;
    pthread_mutex_unlock(&stream->ds->in->lock);
    return r;
}

void demux_set_stream_wakeup_cb(struct sh_stream *sh,
                                void (*cb)(void *ctx), void *ctx)
{
    pthread_mutex_lock(&sh->ds->in->lock);
    assert(cb == NULL || !sh->ds->wakeup_cb);
    sh->ds->wakeup_cb = cb;
    sh->ds->wakeup_cb_ctx = ctx;
    sh->ds->need_wakeup = true;
    pthread_mutex_unlock(&sh->ds->in->lock);
}

int demuxer_add_attachment(demuxer_t *demuxer, char *name, char *type,
                           void *data, size_t data_size)
{
    if (!(demuxer->num_attachments % 32))
        demuxer->attachments = talloc_realloc(demuxer, demuxer->attachments,
                                              struct demux_attachment,
                                              demuxer->num_attachments + 32);

    struct demux_attachment *att = &demuxer->attachments[demuxer->num_attachments];
    att->name = talloc_strdup(demuxer->attachments, name);
    att->type = talloc_strdup(demuxer->attachments, type);
    att->data = talloc_memdup(demuxer->attachments, data, data_size);
    att->data_size = data_size;

    return demuxer->num_attachments++;
}

static int chapter_compare(const void *p1, const void *p2)
{
    struct demux_chapter *c1 = (void *)p1;
    struct demux_chapter *c2 = (void *)p2;

    if (c1->pts > c2->pts)
        return 1;
    else if (c1->pts < c2->pts)
        return -1;
    return c1->original_index > c2->original_index ? 1 :-1; // never equal
}

static void demuxer_sort_chapters(demuxer_t *demuxer)
{
    qsort(demuxer->chapters, demuxer->num_chapters,
          sizeof(struct demux_chapter), chapter_compare);
}

int demuxer_add_chapter(demuxer_t *demuxer, char *name,
                        double pts, uint64_t demuxer_id)
{
    struct demux_chapter new = {
        .original_index = demuxer->num_chapters,
        .pts = pts,
        .metadata = talloc_zero(demuxer, struct mp_tags),
        .demuxer_id = demuxer_id,
    };
    mp_tags_set_str(new.metadata, "TITLE", name);
    MP_TARRAY_APPEND(demuxer, demuxer->chapters, demuxer->num_chapters, new);
    return demuxer->num_chapters - 1;
}

void demux_disable_cache(demuxer_t *demuxer)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    pthread_mutex_lock(&in->lock);
    if (in->seekable_cache) {
        MP_VERBOSE(demuxer, "disabling persistent packet cache\n");
        in->seekable_cache = false;

        // Get rid of potential buffered ranges floating around.
        free_empty_cached_ranges(in);
        // Get rid of potential old packets in the current range.
        prune_old_packets(in);
    }
    pthread_mutex_unlock(&in->lock);
}

// Disallow reading any packets and make readers think there is no new data
// yet, until a seek is issued.
void demux_block_reading(struct demuxer *demuxer, bool block)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    pthread_mutex_lock(&in->lock);
    in->blocked = block;
    for (int n = 0; n < in->num_streams; n++) {
        in->streams[n]->ds->need_wakeup = true;
        wakeup_ds(in->streams[n]->ds);
    }
    pthread_cond_signal(&in->wakeup);
    pthread_mutex_unlock(&in->lock);
}

// must be called not locked
static void update_cache(struct demux_internal *in)
{
    struct demuxer *demuxer = in->d_thread;
    struct stream *stream = demuxer->stream;

    // Don't lock while querying the stream.
    struct mp_tags *stream_metadata = NULL;
    struct stream_cache_info stream_cache_info = {.size = -1};

    int64_t stream_size = stream_get_size(stream);
    stream_control(stream, STREAM_CTRL_GET_METADATA, &stream_metadata);
    stream_control(stream, STREAM_CTRL_GET_CACHE_INFO, &stream_cache_info);

    pthread_mutex_lock(&in->lock);
    in->stream_size = stream_size;
    in->stream_cache_info = stream_cache_info;
    if (stream_metadata) {
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            ds_modify_demux_tags(ds);
            mp_tags_replace(ds->tags_demux->stream, stream_metadata);
        }
        talloc_free(stream_metadata);
    }
    pthread_mutex_unlock(&in->lock);
}

// must be called locked
static int cached_stream_control(struct demux_internal *in, int cmd, void *arg)
{
    // If the cache is active, wake up the thread to possibly update cache state.
    if (in->stream_cache_info.size >= 0) {
        in->force_cache_update = true;
        pthread_cond_signal(&in->wakeup);
    }

    switch (cmd) {
    case STREAM_CTRL_GET_CACHE_INFO:
        if (in->stream_cache_info.size < 0)
            return STREAM_UNSUPPORTED;
        *(struct stream_cache_info *)arg = in->stream_cache_info;
        return STREAM_OK;
    case STREAM_CTRL_GET_SIZE:
        if (in->stream_size < 0)
            return STREAM_UNSUPPORTED;
        *(int64_t *)arg = in->stream_size;
        return STREAM_OK;
    case STREAM_CTRL_GET_BASE_FILENAME:
        if (!in->stream_base_filename)
            return STREAM_UNSUPPORTED;
        *(char **)arg = talloc_strdup(NULL, in->stream_base_filename);
        return STREAM_OK;
    }
    return STREAM_ERROR;
}

// must be called locked
static int cached_demux_control(struct demux_internal *in, int cmd, void *arg)
{
    switch (cmd) {
    case DEMUXER_CTRL_STREAM_CTRL: {
        struct demux_ctrl_stream_ctrl *c = arg;
        int r = cached_stream_control(in, c->ctrl, c->arg);
        if (r == STREAM_ERROR)
            break;
        c->res = r;
        return CONTROL_OK;
    }
    case DEMUXER_CTRL_GET_BITRATE_STATS: {
        double *rates = arg;
        for (int n = 0; n < STREAM_TYPE_COUNT; n++)
            rates[n] = -1;
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            if (ds->selected && ds->bitrate >= 0)
                rates[ds->type] = MPMAX(0, rates[ds->type]) + ds->bitrate;
        }
        return CONTROL_OK;
    }
    case DEMUXER_CTRL_GET_READER_STATE: {
        struct demux_ctrl_reader_state *r = arg;
        *r = (struct demux_ctrl_reader_state){
            .eof = in->last_eof,
            .ts_reader = MP_NOPTS_VALUE,
            .ts_end = MP_NOPTS_VALUE,
            .ts_duration = -1,
            .total_bytes = in->total_bytes,
            .seeking = in->seeking_in_progress,
            .low_level_seeks = in->low_level_seeks,
            .ts_last = in->demux_ts,
        };
        bool any_packets = false;
        for (int n = 0; n < in->num_streams; n++) {
            struct demux_stream *ds = in->streams[n]->ds;
            if (ds->eager && !(!ds->queue->head && ds->eof) && !ds->ignore_eof)
            {
                r->underrun |= !ds->reader_head && !ds->eof && !ds->still_image;
                r->ts_reader = MP_PTS_MAX(r->ts_reader, ds->base_ts);
                r->ts_end = MP_PTS_MAX(r->ts_end, ds->queue->last_ts);
                any_packets |= !!ds->reader_head;
            }
            r->fw_bytes += get_foward_buffered_bytes(ds);
        }
        r->idle = (in->idle && !r->underrun) || r->eof;
        r->underrun &= !r->idle;
        r->ts_reader = MP_ADD_PTS(r->ts_reader, in->ts_offset);
        r->ts_end = MP_ADD_PTS(r->ts_end, in->ts_offset);
        if (r->ts_reader != MP_NOPTS_VALUE && r->ts_reader <= r->ts_end)
            r->ts_duration = r->ts_end - r->ts_reader;
        if (in->seeking || !any_packets)
            r->ts_duration = 0;
        for (int n = 0; n < MPMIN(in->num_ranges, MAX_SEEK_RANGES); n++) {
            struct demux_cached_range *range = in->ranges[n];
            if (range->seek_start != MP_NOPTS_VALUE) {
                r->seek_ranges[r->num_seek_ranges++] =
                    (struct demux_seek_range){
                        .start = MP_ADD_PTS(range->seek_start, in->ts_offset),
                        .end = MP_ADD_PTS(range->seek_end, in->ts_offset),
                    };
            }
        }
        return CONTROL_OK;
    }
    }
    return CONTROL_UNKNOWN;
}

struct demux_control_args {
    struct demuxer *demuxer;
    int cmd;
    void *arg;
    int *r;
};

static void thread_demux_control(void *p)
{
    struct demux_control_args *args = p;
    struct demuxer *demuxer = args->demuxer;
    int cmd = args->cmd;
    void *arg = args->arg;
    struct demux_internal *in = demuxer->in;
    int r = CONTROL_UNKNOWN;

    pthread_mutex_unlock(&in->lock);

    if (cmd == DEMUXER_CTRL_STREAM_CTRL) {
        struct demux_ctrl_stream_ctrl *c = arg;
        if (in->threading)
            MP_VERBOSE(demuxer, "blocking for STREAM_CTRL %d\n", c->ctrl);
        c->res = stream_control(demuxer->stream, c->ctrl, c->arg);
        if (c->res != STREAM_UNSUPPORTED)
            r = CONTROL_OK;
    }
    if (r != CONTROL_OK) {
        if (in->threading)
            MP_VERBOSE(demuxer, "blocking for DEMUXER_CTRL %d\n", cmd);
        if (demuxer->desc->control)
            r = demuxer->desc->control(demuxer->in->d_thread, cmd, arg);
    }

    pthread_mutex_lock(&in->lock);

    *args->r = r;
}

int demux_control(demuxer_t *demuxer, int cmd, void *arg)
{
    struct demux_internal *in = demuxer->in;
    assert(demuxer == in->d_user);

    if (in->threading) {
        pthread_mutex_lock(&in->lock);
        int cr = cached_demux_control(in, cmd, arg);
        pthread_mutex_unlock(&in->lock);
        if (cr != CONTROL_UNKNOWN)
            return cr;
    }

    int r = 0;
    struct demux_control_args args = {demuxer, cmd, arg, &r};
    if (in->threading) {
        MP_VERBOSE(in, "blocking on demuxer thread\n");
        pthread_mutex_lock(&in->lock);
        while (in->run_fn)
            pthread_cond_wait(&in->wakeup, &in->lock);
        in->run_fn = thread_demux_control;
        in->run_fn_arg = &args;
        pthread_cond_signal(&in->wakeup);
        while (in->run_fn)
            pthread_cond_wait(&in->wakeup, &in->lock);
        pthread_mutex_unlock(&in->lock);
    } else {
        pthread_mutex_lock(&in->lock);
        thread_demux_control(&args);
        pthread_mutex_unlock(&in->lock);
    }

    return r;
}

int demux_stream_control(demuxer_t *demuxer, int ctrl, void *arg)
{
    struct demux_ctrl_stream_ctrl c = {ctrl, arg, STREAM_UNSUPPORTED};
    demux_control(demuxer, DEMUXER_CTRL_STREAM_CTRL, &c);
    return c.res;
}

bool demux_cancel_test(struct demuxer *demuxer)
{
    return mp_cancel_test(demuxer->stream->cancel);
}

struct demux_chapter *demux_copy_chapter_data(struct demux_chapter *c, int num)
{
    struct demux_chapter *new = talloc_array(NULL, struct demux_chapter, num);
    for (int n = 0; n < num; n++) {
        new[n] = c[n];
        new[n].metadata = mp_tags_dup(new, new[n].metadata);
    }
    return new;
}
