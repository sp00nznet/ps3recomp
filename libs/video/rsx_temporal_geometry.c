#include "rsx_temporal_geometry.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct rsx_tg_draw {
    uint64_t id, signature;
    size_t offset, count;
} rsx_tg_draw;

typedef struct rsx_tg_frame {
    rsx_tg_draw *draws;
    rsx_tg_clip *clips;
    size_t draw_count, vertex_count;
    uint64_t id;
    uint32_t width, height;
    float jitter_x, jitter_y;
    int sealed;
} rsx_tg_frame;

struct rsx_tg_history {
    rsx_tg_frame frames[2];
    size_t max_draws, max_vertices;
    unsigned current;
    int active, failed;
    rsx_tg_reason base_reason;
};

static int finite_clip(const rsx_tg_clip *c)
{
    return isfinite(c->x) && isfinite(c->y) && isfinite(c->z) &&
           isfinite(c->w);
}

static int valid_clip(const rsx_tg_clip *c)
{
    return finite_clip(c) && c->w > 0.0f && c->z >= 0.0f && c->z <= c->w;
}

static rsx_tg_frame *current_frame(rsx_tg_history *h)
{
    return &h->frames[h->current];
}

static const rsx_tg_draw *find_draw(const rsx_tg_frame *f, uint64_t id)
{
    size_t i;
    for (i = 0; i < f->draw_count; ++i)
        if (f->draws[i].id == id)
            return &f->draws[i];
    return NULL;
}

rsx_tg_result rsx_tg_create(rsx_tg_history **out, size_t max_draws,
                           size_t max_vertices)
{
    rsx_tg_history *h;
    size_t draw_bytes, clip_bytes;
    unsigned i;
    if (!out || !max_draws || !max_vertices ||
        max_draws > SIZE_MAX / sizeof(rsx_tg_draw) ||
        max_vertices > SIZE_MAX / sizeof(rsx_tg_clip))
        return RSX_TG_INVALID_ARGUMENT;
    draw_bytes = max_draws * sizeof(rsx_tg_draw);
    clip_bytes = max_vertices * sizeof(rsx_tg_clip);
    if (draw_bytes > (SIZE_MAX - sizeof(*h)) / 2 ||
        clip_bytes > (SIZE_MAX - sizeof(*h) - 2 * draw_bytes) / 2)
        return RSX_TG_INVALID_ARGUMENT;
    h = (rsx_tg_history *)calloc(1, sizeof(*h));
    if (!h)
        return RSX_TG_OUT_OF_MEMORY;
    for (i = 0; i < 2; ++i) {
        h->frames[i].draws = (rsx_tg_draw *)calloc(max_draws, sizeof(rsx_tg_draw));
        h->frames[i].clips = (rsx_tg_clip *)malloc(max_vertices * sizeof(rsx_tg_clip));
        if (!h->frames[i].draws || !h->frames[i].clips) {
            rsx_tg_destroy(h);
            return RSX_TG_OUT_OF_MEMORY;
        }
    }
    h->max_draws = max_draws;
    h->max_vertices = max_vertices;
    h->base_reason = RSX_TG_NO_PREVIOUS_FRAME;
    *out = h;
    return RSX_TG_OK;
}

void rsx_tg_destroy(rsx_tg_history *h)
{
    unsigned i;
    if (!h)
        return;
    for (i = 0; i < 2; ++i) {
        free(h->frames[i].draws);
        free(h->frames[i].clips);
    }
    free(h);
}

void rsx_tg_reset(rsx_tg_history *h)
{
    unsigned i;
    if (!h)
        return;
    for (i = 0; i < 2; ++i) {
        h->frames[i].sealed = 0;
        h->frames[i].draw_count = 0;
        h->frames[i].vertex_count = 0;
    }
    h->active = 0;
    h->failed = 0;
    h->base_reason = RSX_TG_NO_PREVIOUS_FRAME;
}

rsx_tg_result rsx_tg_begin_frame(rsx_tg_history *h, uint64_t frame_id,
                                 uint32_t width, uint32_t height,
                                 float jitter_x, float jitter_y, int camera_cut)
{
    rsx_tg_frame *old, *now;
    rsx_tg_reason reason = RSX_TG_NO_PREVIOUS_FRAME;
    if (!h || !width || !height || !isfinite(jitter_x) || !isfinite(jitter_y))
        return RSX_TG_INVALID_ARGUMENT;
    old = current_frame(h);
    if (h->active) {
        old->sealed = 0; /* Unfinished frame never becomes a reference. */
        reason = RSX_TG_HISTORY_FAILED;
    } else if (old->sealed) {
        if (camera_cut)
            reason = RSX_TG_CAMERA_CUT;
        else if (old->id == UINT64_MAX || frame_id != old->id + 1)
            reason = RSX_TG_FRAME_DISCONTINUITY;
        else if (old->width != width || old->height != height)
            reason = RSX_TG_EXTENT_CHANGED;
        else
            reason = RSX_TG_VALID;
    }
    h->current ^= 1u;
    now = current_frame(h);
    now->id = frame_id;
    now->width = width;
    now->height = height;
    now->jitter_x = jitter_x;
    now->jitter_y = jitter_y;
    now->draw_count = 0;
    now->vertex_count = 0;
    now->sealed = 0;
    h->active = 1;
    h->failed = 0;
    h->base_reason = reason;
    return RSX_TG_OK;
}

rsx_tg_result rsx_tg_append_draw(rsx_tg_history *h, uint64_t draw_id,
                                 uint64_t signature, const rsx_tg_clip *clips,
                                 size_t count)
{
    rsx_tg_frame *f;
    size_t i;
    rsx_tg_draw *d;
    if (!h || !h->active)
        return RSX_TG_BAD_STATE;
    if (h->failed)
        return RSX_TG_BAD_STATE;
    f = current_frame(h);
    if (!draw_id || !clips || !count) {
        h->failed = 1;
        return RSX_TG_INVALID_ARGUMENT;
    }
    if (find_draw(f, draw_id)) {
        h->failed = 1;
        return RSX_TG_DUPLICATE_ID;
    }
    if (f->draw_count >= h->max_draws ||
        count > h->max_vertices - f->vertex_count) {
        h->failed = 1;
        return RSX_TG_CAPACITY;
    }
    for (i = 0; i < count; ++i) {
        if (!finite_clip(&clips[i])) {
            h->failed = 1;
            return RSX_TG_INVALID_ARGUMENT;
        }
    }
    d = &f->draws[f->draw_count++];
    d->id = draw_id;
    d->signature = signature;
    d->offset = f->vertex_count;
    d->count = count;
    memcpy(&f->clips[d->offset], clips, count * sizeof(*clips));
    f->vertex_count += count;
    return RSX_TG_OK;
}

rsx_tg_result rsx_tg_match_draw(const rsx_tg_history *h, uint64_t draw_id,
                                rsx_tg_match *out)
{
    const rsx_tg_frame *cur, *prev;
    const rsx_tg_draw *cd, *pd;
    if (!h || !out || !draw_id)
        return RSX_TG_INVALID_ARGUMENT;
    if (!h->active)
        return RSX_TG_BAD_STATE;
    if (h->failed) {
        out->reason = RSX_TG_HISTORY_FAILED;
        return RSX_TG_UNAVAILABLE;
    }
    cur = &h->frames[h->current];
    cd = find_draw(cur, draw_id);
    if (!cd) {
        out->reason = RSX_TG_MISSING_DRAW;
        return RSX_TG_UNAVAILABLE;
    }
    if (h->base_reason != RSX_TG_VALID) {
        out->reason = h->base_reason;
        return RSX_TG_UNAVAILABLE;
    }
    prev = &h->frames[h->current ^ 1u];
    pd = find_draw(prev, draw_id);
    if (!pd) {
        out->reason = RSX_TG_MISSING_DRAW;
        return RSX_TG_UNAVAILABLE;
    }
    if (cd->signature != pd->signature || cd->count != pd->count) {
        out->reason = RSX_TG_TOPOLOGY_CHANGED;
        return RSX_TG_UNAVAILABLE;
    }
    out->current = &cur->clips[cd->offset];
    out->previous = &prev->clips[pd->offset];
    out->vertex_count = cd->count;
    out->width = cur->width;
    out->height = cur->height;
    out->current_jitter_x = cur->jitter_x;
    out->current_jitter_y = cur->jitter_y;
    out->previous_jitter_x = prev->jitter_x;
    out->previous_jitter_y = prev->jitter_y;
    out->reason = RSX_TG_VALID;
    return RSX_TG_OK;
}

rsx_tg_result rsx_tg_end_frame(rsx_tg_history *h)
{
    if (!h || !h->active)
        return RSX_TG_BAD_STATE;
    h->active = 0;
    current_frame(h)->sealed = !h->failed;
    return h->failed ? RSX_TG_BAD_STATE : RSX_TG_OK;
}

rsx_tg_result rsx_tg_triangle_motion(const rsx_tg_clip current[3],
                                     const rsx_tg_clip previous[3],
                                     const float bary[3],
                                     uint32_t width, uint32_t height,
                                     float current_jitter_x,
                                     float current_jitter_y,
                                     float previous_jitter_x,
                                     float previous_jitter_y,
                                     rsx_tg_motion *out)
{
    double q[3], sum = 0.0, bsum = 0.0;
    double cx = 0.0, cy = 0.0, cz = 0.0, cw = 0.0;
    double px = 0.0, py = 0.0, pz = 0.0, pw = 0.0;
    double mx, my;
    unsigned i;
    if (!current || !previous || !bary || !out || !width || !height ||
        !isfinite(current_jitter_x) || !isfinite(current_jitter_y) ||
        !isfinite(previous_jitter_x) || !isfinite(previous_jitter_y))
        return RSX_TG_INVALID_ARGUMENT;
    for (i = 0; i < 3; ++i) {
        if (!valid_clip(&current[i]) || !valid_clip(&previous[i]) ||
            !isfinite(bary[i]) || bary[i] < -1e-5f || bary[i] > 1.00001f)
            return RSX_TG_INVALID_ARGUMENT;
        bsum += (double)bary[i];
    }
    if (fabs(bsum - 1.0) > 1e-5)
        return RSX_TG_INVALID_ARGUMENT;
    for (i = 0; i < 3; ++i) {
        q[i] = ((double)bary[i] / bsum) / (double)current[i].w;
        sum += q[i];
    }
    if (!isfinite(sum) || sum <= 0.0)
        return RSX_TG_INVALID_ARGUMENT;
    for (i = 0; i < 3; ++i) {
        double t = q[i] / sum;
        cx += t * current[i].x; cy += t * current[i].y;
        cz += t * current[i].z; cw += t * current[i].w;
        px += t * previous[i].x; py += t * previous[i].y;
        pz += t * previous[i].z; pw += t * previous[i].w;
    }
    if (!isfinite(cw) || !isfinite(pw) || cw <= 0.0 || pw <= 0.0 ||
        !isfinite(cz) || !isfinite(pz) || cz < 0.0 || cz > cw ||
        pz < 0.0 || pz > pw)
        return RSX_TG_INVALID_ARGUMENT;
    mx = ((px / pw) - (cx / cw)) * ((double)width * 0.5) +
         (double)current_jitter_x - (double)previous_jitter_x;
    my = ((cy / cw) - (py / pw)) * ((double)height * 0.5) +
         (double)current_jitter_y - (double)previous_jitter_y;
    if (!isfinite(mx) || !isfinite(my) || fabs(mx) > FLT_MAX || fabs(my) > FLT_MAX)
        return RSX_TG_INVALID_ARGUMENT;
    out->x = (float)mx;
    out->y = (float)my;
    return RSX_TG_OK;
}
