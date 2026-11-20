#include "avl.h"

#include <stdlib.h>

struct avl_node {
    int32_t   key;
    uint32_t  val;
    int       height;
    avl_node *left;
    avl_node *right;
};

static int h(const avl_node *n) { return n ? n->height : 0; }
static int max_int(int a, int b) { return a > b ? a : b; }
static void fix_height(avl_node *n) { n->height = 1 + max_int(h(n->left), h(n->right)); }
static int balance_of(const avl_node *n) { return h(n->left) - h(n->right); }

static avl_node *rotate_right(avl_node *y)
{
    avl_node *x = y->left;
    y->left = x->right;
    x->right = y;
    fix_height(y);
    fix_height(x);
    return x;
}

static avl_node *rotate_left(avl_node *x)
{
    avl_node *y = x->right;
    x->right = y->left;
    y->left = x;
    fix_height(x);
    fix_height(y);
    return y;
}

/* Restore the AVL invariant at n (|balance| <= 1) after an insert/delete below it. */
static avl_node *rebalance(avl_node *n)
{
    fix_height(n);
    int bf = balance_of(n);
    if (bf > 1) {
        if (balance_of(n->left) < 0)          /* left-right case */
            n->left = rotate_left(n->left);
        return rotate_right(n);               /* left-left case */
    }
    if (bf < -1) {
        if (balance_of(n->right) > 0)         /* right-left case */
            n->right = rotate_right(n->right);
        return rotate_left(n);                /* right-right case */
    }
    return n;
}

void avl_init(avl_tree *t)
{
    t->root = NULL;
    t->count = 0;
}

static void free_nodes(avl_node *n)
{
    /* Iterative post-order-free via right rotations: O(n), no recursion. */
    while (n) {
        if (n->left) {
            avl_node *l = n->left;
            n->left = l->right;
            l->right = n;
            n = l;
        } else {
            avl_node *r = n->right;
            free(n);
            n = r;
        }
    }
}

void avl_free(avl_tree *t)
{
    free_nodes(t->root);
    t->root = NULL;
    t->count = 0;
}

static avl_node *insert_rec(avl_node *n, int32_t key, uint32_t val, int *status)
{
    if (!n) {
        avl_node *fresh = malloc(sizeof *fresh);
        if (!fresh) {
            *status = -1;
            return NULL;
        }
        fresh->key = key;
        fresh->val = val;
        fresh->height = 1;
        fresh->left = fresh->right = NULL;
        return fresh;
    }
    if (key < n->key) {
        avl_node *child = insert_rec(n->left, key, val, status);
        if (*status != 0)
            return n;
        n->left = child;
    } else if (key > n->key) {
        avl_node *child = insert_rec(n->right, key, val, status);
        if (*status != 0)
            return n;
        n->right = child;
    } else {
        *status = 1;
        return n;
    }
    return rebalance(n);
}

int avl_insert(avl_tree *t, int32_t key, uint32_t val)
{
    int status = 0;
    avl_node *root = insert_rec(t->root, key, val, &status);
    if (status == 0) {
        t->root = root;
        t->count++;
    }
    return status;
}

static avl_node *build_sorted_rec(const avl_kv *items, size_t lo, size_t hi, int *oom)
{
    if (lo >= hi || *oom)
        return NULL;
    size_t mid = lo + (hi - lo) / 2;
    avl_node *n = malloc(sizeof *n);
    if (!n) {
        *oom = 1;
        return NULL;
    }
    n->key = items[mid].key;
    n->val = items[mid].val;
    n->left = build_sorted_rec(items, lo, mid, oom);
    n->right = build_sorted_rec(items, mid + 1, hi, oom);
    fix_height(n);
    return n;
}

int avl_build_sorted(avl_tree *t, const avl_kv *items, size_t n)
{
    if (!t || (n > 0 && !items))
        return -1;
    for (size_t i = 1; i < n; i++) {
        if (items[i - 1].key >= items[i].key)
            return -1;   /* must be strictly ascending */
    }
    avl_free(t);
    if (n == 0)
        return 0;
    int oom = 0;
    avl_node *root = build_sorted_rec(items, 0, n, &oom);
    if (oom) {
        free_nodes(root);
        return -1;
    }
    t->root = root;
    t->count = n;
    return 0;
}

int avl_find(const avl_tree *t, int32_t key, uint32_t *out)
{
    const avl_node *n = t->root;
    while (n) {
        if (key < n->key)
            n = n->left;
        else if (key > n->key)
            n = n->right;
        else {
            if (out)
                *out = n->val;
            return 1;
        }
    }
    return 0;
}

int avl_set(avl_tree *t, int32_t key, uint32_t val)
{
    avl_node *n = t->root;
    while (n) {
        if (key < n->key)
            n = n->left;
        else if (key > n->key)
            n = n->right;
        else {
            n->val = val;
            return 1;
        }
    }
    return 0;
}

static avl_node *remove_min(avl_node *n, avl_node **min_out)
{
    if (!n->left) {
        *min_out = n;
        return n->right;
    }
    n->left = remove_min(n->left, min_out);
    return rebalance(n);
}

static avl_node *remove_rec(avl_node *n, int32_t key, int *removed)
{
    if (!n)
        return NULL;
    if (key < n->key) {
        n->left = remove_rec(n->left, key, removed);
    } else if (key > n->key) {
        n->right = remove_rec(n->right, key, removed);
    } else {
        *removed = 1;
        avl_node *l = n->left, *r = n->right;
        free(n);
        if (!r)
            return l;
        avl_node *succ;
        avl_node *rest = remove_min(r, &succ);   /* in-order successor replaces n */
        succ->right = rest;
        succ->left = l;
        return rebalance(succ);
    }
    return rebalance(n);
}

int avl_remove(avl_tree *t, int32_t key)
{
    int removed = 0;
    t->root = remove_rec(t->root, key, &removed);
    if (removed)
        t->count--;
    return removed;
}

/* Iterative in-order range walk over [lo, hi] using an explicit bounded node stack.
 * Eliminates C call-stack recursion while pruning subtrees outside [lo, hi]. */
size_t avl_range(const avl_tree *t, int32_t lo, int32_t hi, avl_visit_fn fn, void *ctx)
{
    if (!t || !t->root || lo > hi)
        return 0;

    const avl_node *stack[64];
    int top = 0;
    const avl_node *curr = t->root;
    size_t visited = 0;

    while (curr || top > 0) {
        while (curr) {
            stack[top++] = curr;
            curr = (lo < curr->key) ? curr->left : NULL;
        }
        curr = stack[--top];
        if (curr->key >= lo && curr->key <= hi) {
            visited++;
            if (fn && fn(curr->key, curr->val, ctx))
                return visited;
        }
        curr = (hi > curr->key) ? curr->right : NULL;
    }
    return visited;
}

int avl_height(const avl_tree *t) { return h(t->root); }

static int validate_rec(const avl_node *n, int64_t lo, int64_t hi, int *ok)
{
    if (!n)
        return 0;
    if ((int64_t)n->key <= lo || (int64_t)n->key >= hi)
        *ok = 0;
    int lh = validate_rec(n->left, lo, n->key, ok);
    int rh = validate_rec(n->right, n->key, hi, ok);
    int bf = lh - rh;
    if (bf > 1 || bf < -1)
        *ok = 0;
    int real = 1 + max_int(lh, rh);
    if (real != n->height)
        *ok = 0;
    return real;
}

int avl_validate(const avl_tree *t)
{
    int ok = 1;
    validate_rec(t->root, (int64_t)INT32_MIN - 1, (int64_t)INT32_MAX + 1, &ok);
    return ok;
}
