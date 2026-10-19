#include "rsx_scene_bvh.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TRIANGLE_LIMIT 1000000u
#define LEAF_SIZE 4u
#define STACK_SIZE 64u
typedef struct triangle {
    float p[9];
    double center[3];
    uint32_t original;
} triangle;
typedef struct node {
    float lo[3], hi[3];
    uint32_t first, count, left, right;
} node;
struct rsx_scene_bvh {
    triangle* triangles;
    node* nodes;
    uint32_t count, used;
};

/* Local heapsort avoids qsort comparator globals, unstable equal keys, and
 * adversarial quickselect behavior. Median splits bound depth to <= 20. */
static int less(const triangle* a, const triangle* b, unsigned axis)
{
    return a->center[axis] < b->center[axis] ||
        (a->center[axis] == b->center[axis] && a->original < b->original);
}
static void swap(triangle* a, triangle* b)
{
    triangle tmp = *a; *a = *b; *b = tmp;
}
static void sift(triangle* a, size_t root, size_t count, unsigned axis)
{
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && less(&a[child], &a[child + 1], axis)) ++child;
        if (!less(&a[root], &a[child], axis)) break;
        swap(&a[root], &a[child]); root = child;
    }
}
static void sort(triangle* a, size_t count, unsigned axis)
{
    for (size_t i = count / 2; i > 0; --i) sift(a, i - 1, count, axis);
    for (size_t i = count; i > 1; --i) {
        swap(&a[0], &a[i - 1]); sift(a, 0, i - 1, axis);
    }
}
static uint32_t build(rsx_scene_bvh* b, uint32_t first, uint32_t count)
{
    uint32_t index = b->used++;
    node* n = &b->nodes[index];
    n->first = first; n->count = count;
    for (unsigned k = 0; k < 3; ++k) {
        n->lo[k] = n->hi[k] = b->triangles[first].p[k];
        for (uint32_t i = first; i < first + count; ++i)
            for (unsigned v = 0; v < 3; ++v) {
                float p = b->triangles[i].p[v * 3 + k];
                if (p < n->lo[k]) n->lo[k] = p;
                if (p > n->hi[k]) n->hi[k] = p;
            }
    }
    if (count > LEAF_SIZE) {
        unsigned axis = 0;
        for (unsigned k = 1; k < 3; ++k)
            if ((double)n->hi[k] - n->lo[k] >
                (double)n->hi[axis] - n->lo[axis]) axis = k;
        sort(b->triangles + first, count, axis);
        uint32_t half = count / 2;
        n->left = build(b, first, half);
        n->right = build(b, first + half, count - half);
        n->count = 0;
    }
    return index;
}
rsx_scene_bvh* rsx_scene_bvh_create(const float* input, size_t count)
{
    if (count > TRIANGLE_LIMIT || (count && !input) ||
        count > SIZE_MAX / sizeof(triangle) ||
        count > SIZE_MAX / (2 * sizeof(node)) || count > SIZE_MAX / 9) return NULL;
    for (size_t i = 0; i < count * 9; ++i) if (!isfinite(input[i])) return NULL;
    rsx_scene_bvh* b = (rsx_scene_bvh*)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->count = (uint32_t)count;
    if (!count) return b;
    b->triangles = (triangle*)malloc(count * sizeof(triangle));
    b->nodes = (node*)calloc(count * 2, sizeof(node));
    if (!b->triangles || !b->nodes) { rsx_scene_bvh_destroy(b); return NULL; }
    for (size_t i = 0; i < count; ++i) {
        triangle* t = &b->triangles[i];
        memcpy(t->p, input + i * 9, sizeof(t->p));
        t->original = (uint32_t)i;
        for (unsigned k = 0; k < 3; ++k)
            t->center[k] = ((double)t->p[k] + t->p[k + 3] + t->p[k + 6]) / 3;
    }
    build(b, 0, (uint32_t)count);
    return b;
}
void rsx_scene_bvh_destroy(rsx_scene_bvh* b)
{
    if (b) { free(b->triangles); free(b->nodes); free(b); }
}
static int box_hit(const node* n, const float* o, const float* d, double low, double high)
{
    for (unsigned k = 0; k < 3; ++k) {
        if (d[k] == 0) {
            if (o[k] < n->lo[k] || o[k] > n->hi[k]) return 0;
        } else {
            double a = ((double)n->lo[k] - o[k]) / d[k];
            double z = ((double)n->hi[k] - o[k]) / d[k];
            if (a > z) { double tmp = a; a = z; z = tmp; }
            if (a > low) low = a;
            if (z < high) high = z;
            if (low > high) return 0;
        }
    }
    return 1;
}
static void cross(const double* a, const double* b, double* c)
{
    c[0] = a[1]*b[2] - a[2]*b[1];
    c[1] = a[2]*b[0] - a[0]*b[2];
    c[2] = a[0]*b[1] - a[1]*b[0];
}
static double dot(const double* a, const double* b)
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static int triangle_hit(const triangle* tri, const float* o, const float* d,
                        double low, double high, double* distance,
                        rsx_scene_bvh_hit* hit)
{
    double e1[3], e2[3], ray[3], delta[3], p[3], q[3], normal[3];
    for (unsigned k = 0; k < 3; ++k) {
        e1[k] = (double)tri->p[k + 3] - tri->p[k];
        e2[k] = (double)tri->p[k + 6] - tri->p[k];
        ray[k] = d[k]; delta[k] = (double)o[k] - tri->p[k];
    }
    cross(e1, e2, normal);
    double length = sqrt(dot(normal, normal));
    if (length == 0) return 0;
    cross(ray, e2, p);
    double det = dot(e1, p);
    if (det == 0) return 0;
    double u = dot(delta, p) / det;
    if (u < 0 || u > 1) return 0;
    cross(delta, e1, q);
    double v = dot(ray, q) / det;
    if (v < 0 || u + v > 1) return 0;
    double t = dot(e2, q) / det;
    if (t < low || t > high || !isfinite(t)) return 0;
    *distance = t;
    hit->triangle_index = tri->original;
    hit->t = (float)t; hit->u = (float)u; hit->v = (float)v;
    for (unsigned k = 0; k < 3; ++k) hit->normal[k] = (float)(normal[k] / length);
    return 1;
}
int rsx_scene_bvh_intersect(const rsx_scene_bvh* b, const float o[3], const float d[3],
                            float low, float high, rsx_scene_bvh_hit* hit)
{
    if (!b || !o || !d || !hit || !isfinite(low) || !isfinite(high) || low > high) return 0;
    for (unsigned k = 0; k < 3; ++k) if (!isfinite(o[k]) || !isfinite(d[k])) return 0;
    if ((!d[0] && !d[1] && !d[2]) || !b->count) return 0;
    uint32_t stack[STACK_SIZE];
    size_t pending = 0;
    stack[pending++] = 0;
    int found = 0;
    double best = high;
    rsx_scene_bvh_hit result = {0};
    while (pending) {
        const node* n = &b->nodes[stack[--pending]];
        if (!box_hit(n, o, d, low, best)) continue;
        if (n->count) {
            for (uint32_t i = n->first; i < n->first + n->count; ++i) {
                rsx_scene_bvh_hit candidate;
                double t;
                if (triangle_hit(&b->triangles[i], o, d, low, best, &t, &candidate) &&
                    (!found || t < best || candidate.triangle_index < result.triangle_index)) {
                    best = t; result = candidate; found = 1;
                }
            }
        } else {
            /* Construction's balanced depth is below 20; defensive guard keeps
             * this bounded even if the implementation's cap changes later. */
            if (pending > STACK_SIZE - 2) return 0;
            stack[pending++] = n->right; stack[pending++] = n->left;
        }
    }
    if (found) *hit = result;
    return found;
}
