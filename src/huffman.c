
#pragma GCC optimize ("O3")

#define printf printk

static uint8_t buffer[7096];

static int verbose = 0;

int hhh = 1023;

/* 8-bit alphabet plus an escape code for emitting symbols not represented in
 * the Huffman tree, and an end-of-stream code to exit the decoder. */
#define NR_SYMBOLS 258
#define SYM_ESC    256
#define SYM_EOS    257

/* LUT: 8-bit code prefix -> Huffman tree node or leaf. */
#define lent_codelen(e) ((e)>>16)
#define lent_node(e) ((uint16_t)(e))
#define mk_lent(node, codelen) (((codelen)<<16)|(node))
typedef uint32_t lent_t;
typedef lent_t *lut_t;

/* Dict: input symbol -> { code, codelen }. */
#define dent_codelen(e) ((e)>>16)
#define dent_code(e) ((uint16_t)(e))
#define mk_dent(code, codelen) (((codelen)<<16)|(code))
typedef uint32_t dent_t;
typedef dent_t *dict_t;

/* A node can be a leaf or internal. Only internal nodes have child links. */
#define NODE_INTERNAL 0x8000
#define node_is_internal(n) ((n) & NODE_INTERNAL)
#define node_is_leaf(n) !node_is_internal(n)
#define node_idx(n) ((n) & 0x7fff)

/* Internal Huffman tree node. */
#define node_left(e) ((e)>>16)
#define node_right(e) ((uint16_t)(e))
#define mk_node(l,r) (((l)<<16)|(r))
typedef uint32_t node_t;

/* Heap: Min-heap used for constructing the Huffman tree. */
#define hent_count(e) ((uint16_t)(e))
#define hent_node(e) ((e)>>16)
#define mk_hent(node, count) (((node)<<16)|(count))
typedef uint32_t hent_t;
typedef hent_t *heap_t; /* [0] -> nr */

struct huffman_state {
    node_t nodes[NR_SYMBOLS]; /* 258 * 4 bytes */
    union {
        hent_t heap[NR_SYMBOLS+1]; /* 259 * 4 bytes */
        lent_t lut[256]; /* 256 * 4 bytes */
        dent_t dict[NR_SYMBOLS]; /* 258 * 4 bytes */
    } u; /* 259 * 4 bytes */
}; /* 517 * 4 = 2068 bytes */
static struct huffman_state huffman_state;

/* Percolate item @i downwards to correct position among subheaps. */
static void heap_percolate_down(heap_t heap, unsigned int i)
{
    unsigned int nr = heap[0];
    uint32_t x = heap[i];
    for (;;) {
        unsigned int l = 2*i, r = 2*i+1, smallest = i;
        uint32_t s = x;
        /* Find the smallest of three nodes. */
        if (likely(l <= nr) && (hent_count(heap[l]) < hent_count(s))) {
            smallest = l;
            s = heap[l];
        }
        if (likely(r <= nr) && (hent_count(heap[r]) < hent_count(s))) {
            smallest = r;
            s = heap[r];
        }
        if (smallest == i)
            break;
        /* Swap with smallest subtree root. */
        heap[i] = s;
        heap[smallest] = x;
        /* Iterate into the subtree we swapped with. */
        i = smallest;
    }
}

static void build_heap(heap_t heap, unsigned int nr)
{
    unsigned int i, j;
    for (i = j = 1; i <= nr; i++) {
        uint32_t he = heap[i];
        if (hent_count(he) != 0)
            heap[j++] = he;
    }
    heap[0] = --j;
    for (i = j/2; i > 0; i--)
        heap_percolate_down(heap, i);
}

static uint16_t build_huffman_tree(heap_t heap, node_t *nodes)
{
    unsigned int nr = heap[0];
    uint32_t x, y;

    for (;;) {
        /* heap_get_min #1 */
        x = heap[1];
        heap[1] = heap[nr];
        if (unlikely((heap[0] = --nr) == 0))
            break;
        heap_percolate_down(heap, 1);
        /* heap_get_min #2 */
        y = heap[1];
        nodes[nr] = mk_node(hent_node(x), hent_node(y));
        heap[1] = mk_hent(nr|NODE_INTERNAL, hent_count(x) + hent_count(y));
        heap_percolate_down(heap, 1);
    }

    return hent_node(x);
}

static uint16_t build_huffman_heap_and_tree(
    unsigned char *model_p, unsigned int model_nr, heap_t heap, node_t *nodes,
    time_t *t)
{
    uint32_t *h = &heap[1];
    unsigned int i;

    t[0] = time_now();
    for (i = 0; i < 256; i++)
        h[i] = mk_hent(i, 0);
    h[SYM_ESC] = mk_hent(SYM_ESC, 1);
    h[SYM_EOS] = mk_hent(SYM_EOS, 1);
    t[1] = time_now();
    for (i = 0; i < model_nr; i++)
        h[(unsigned int)model_p[i]]++;
    t[2] = time_now();

    if (verbose) {
        printf("Frequencies:\n");
        for (i = 0; i < 256; i++)
            if (hent_count(h[i]))
                printf("%03x: %d\n", i, hent_count(h[i]));
        printf("\n");
    }

    build_heap(heap, NR_SYMBOLS);
    t[3] = time_now();
    return build_huffman_tree(heap, nodes);
}

static char *prefix_str(uint32_t prefix, unsigned int prefix_len)
{
    static char s[33];
    int i;
    s[prefix_len] = '\0';
    for (i = prefix_len; i > 0; i--) {
        s[i-1] = '0' + (prefix&1);
        prefix >>= 1;
    }
    return s;
}

static void build_huffman_dict(uint16_t root, node_t *nodes, dict_t dict)
{
    uint16_t stack[32], node = root;
    unsigned int sp = 0, prefix_len = 0;
    uint32_t prefix = 0;

    memset(dict, 0, NR_SYMBOLS * sizeof(*dict));

    for (;;) {

        if (node_is_leaf(node)) {

            /* Visit leaf */
            dict[node] = mk_dent(prefix, prefix_len);
            if (verbose)
                printf("%03x: %d %s\n", node,
                       prefix_len, prefix_str(prefix, prefix_len));

            /* Visit ancestors until we follow a left-side link. */
            do {
                if (sp == 0) {
                    /* Returned to root via right-side link. We're done. */
                    return;
                }
                node = stack[--sp];
                prefix >>= 1;
                prefix_len--;
            } while (node == 0);

            /* Walk right-side link. Dummy on stack for tracking prefix. */
            stack[sp++] = 0;
            node = node_right(nodes[node_idx(node)]);
            prefix = (prefix<<1)|1;

        } else {

            /* Walk left-side link. */
            stack[sp++] = node;
            node = node_left(nodes[node_idx(node)]);
            prefix <<= 1;

        }

        prefix_len++;

    }
}

static void build_huffman_lut(uint16_t root, node_t *nodes, lut_t lut)
{
    uint16_t stack[32], node = root;
    unsigned int sp = 0, prefix_len = 0;
    uint32_t prefix = 0;

    for (;;) {

        if (node_is_leaf(node)) {

            /* Visit leaf */
            int idx = prefix << (8-prefix_len);
            int nr = 1 << (8-prefix_len);
            while (nr--)
                lut[idx+nr] = mk_lent(node, prefix_len);

        up:
            /* Visit ancestors until we follow a left-side link. */
            do {
                if (sp == 0) {
                    /* Returned to root via right-side link. We're done. */
                    return;
                }
                node = stack[--sp];
                prefix >>= 1;
                prefix_len--;
            } while (node == 0);

            /* Walk right-side link. Dummy on stack for tracking prefix. */
            stack[sp++] = 0;
            node = node_right(nodes[node_idx(node)]);
            prefix = (prefix<<1)|1;

        } else if (prefix_len == 8) {

            /* Reached max depth for LUT. */
            lut[prefix] = mk_lent(node, prefix_len);
            goto up;

        } else {

            /* Walk left-side link. */
            stack[sp++] = node;
            node = node_left(nodes[node_idx(node)]);
            prefix <<= 1;

        }

        prefix_len++;

    }
}

static int huffman_compress(
    struct huffman_state *state,
    unsigned char *model_p, unsigned int model_nr,
    unsigned char *msg_p, unsigned int msg_nr,
    unsigned char *out_p)
{
    dent_t dent;
    dict_t dict = state->u.dict;
    unsigned char *p, *q;
    unsigned int i, tot, root, bits;
    uint32_t x;
    time_t t[10];

    /* Verbatim please */
    if (model_p == NULL)
        goto verbatim;

    root = build_huffman_heap_and_tree(
        model_p, model_nr, state->u.heap, state->nodes, t);
    t[4] = time_now();
    build_huffman_dict(root, state->nodes, dict);
    t[5] = time_now();

    x = bits = 0;
    p = out_p + 2;
    q = p + msg_nr;
    for (i = 0; i < msg_nr; i++) {
        unsigned int symbol = msg_p[i];
        if (unlikely((dent = dict[symbol]) == 0)) {
            dent = dict[SYM_ESC];
            x <<= dent_codelen(dent) + 8;
            x |= ((uint32_t)dent_code(dent) << 8) | symbol;
            bits += dent_codelen(dent) + 8;
        } else {
            x <<= dent_codelen(dent);
            x |= dent_code(dent);
            bits += dent_codelen(dent);
        }
        while (bits >= 8) {
            bits -= 8;
            *p++ = x >> bits;
        }
        if (unlikely(p >= q)) goto verbatim;
    }

    dent = dict[SYM_EOS];
    x <<= dent_codelen(dent);
    x |= dent_code(dent);
    bits += dent_codelen(dent);
    while (bits >= 8) {
        bits -= 8;
        *p++ = x >> bits;
    }
    if (bits)
        *p++ = x << (8 - bits);

    tot = p - out_p;
    out_p[0] = tot >> 8;
    out_p[1] = tot;
    if (tot > msg_nr+2) {
    verbatim:
        tot = msg_nr + 2;
        out_p[0] = (tot >> 8) | 0x80;
        out_p[1] = tot;
        memcpy(&out_p[2], msg_p, msg_nr);
    }

    t[6] = time_now();
    printf("init:%d count:%d heap:%d tree:%d tab:%d codec:%d\n",
           (t[1]-t[0])/TIME_MHZ,
           (t[2]-t[1])/TIME_MHZ,
           (t[3]-t[2])/TIME_MHZ,
           (t[4]-t[3])/TIME_MHZ,
           (t[5]-t[4])/TIME_MHZ,
           (t[6]-t[5])/TIME_MHZ);

    return tot;
}

static int huffman_decompress(
    struct huffman_state *state,
    unsigned char *model_p, unsigned int model_nr,
    unsigned char *msg_p, unsigned int msg_nr,
    unsigned char *out_p)
{
    lut_t lut = state->u.lut;
    node_t *nodes = state->nodes;
    unsigned char *p = msg_p, *q = out_p;
    unsigned int root, bits, node;
    uint32_t x;
    time_t t[10];
    unsigned int j = 0;

    root = build_huffman_heap_and_tree(
        model_p, model_nr, state->u.heap, nodes, t);
    t[4] = time_now();
    build_huffman_lut(root, nodes, lut);
    t[5] = time_now();

    x = bits = 0;
    for (;;) {

        uint32_t entry;
        unsigned int codelen;

        while (bits < 24) {
            x |= (uint32_t)(*p++) << (24 - bits);
            bits += 8;
        }

        entry = lut[x >> 24];
        node = lent_node(entry);
        codelen = lent_codelen(entry);
        x <<= codelen; bits -= codelen;

        if (likely(node < 256))
            goto fast_path;

        while (likely(node_is_internal(node))) {
            entry = nodes[node_idx(node)];
            node = (int32_t)x < 0 ? node_right(entry) : node_left(entry);
            x <<= 1; bits--;
        }

        if (likely(node < 256)) {
        fast_path:
            q[j++&hhh] = node;
            continue;
        }

        switch (node) {
        case SYM_EOS:
            goto out;
        case SYM_ESC:
            q[j++&hhh] = x >> 24;
            x <<= 8; bits -= 8;
            break;
        }

    }

out:
    t[6] = time_now();
    printf("init:%d count:%d heap:%d tree:%d tab:%d codec:%d %d\n",
           (t[1]-t[0])/TIME_MHZ,
           (t[2]-t[1])/TIME_MHZ,
           (t[3]-t[2])/TIME_MHZ,
           (t[4]-t[3])/TIME_MHZ,
           (t[5]-t[4])/TIME_MHZ,
           (t[6]-t[5])/TIME_MHZ);
    return q - out_p;
}

void test_huffman(void)
{
#define NR 4000
    unsigned char *p;
    int header, nr;
    time_t t;

    int i,j;
    unsigned char *q = &buffer[4000];
    for (i = 0; i < 256; i++)
        *q++ = i;
    for (j = 0; j < 2048; j++)
        *q++ = _stext[j+1024];
    q = &buffer[4000];

    /* COMPRESS */
    t = time_now();
    nr = huffman_compress(&huffman_state,
                          q, NR, (unsigned char *)_stext+1204, NR,
                          &buffer[0]);

    t = time_now()-t;
    printf("FINAL: %d bytes "
           "Original = %d bytes %d cy, %d us\n",
           nr, NR, t, t/TIME_MHZ);

    /* DECOMPRESS */
    t = time_now();
    p = buffer;
    header = (p[0] << 8) | p[1];
    if (header & (1u<<15)) {
        /* verbatim */
        header &= 0x7fff;
        nr = header - 2;
        printk("Verbatim %d\n", nr);
        
    } else {
        /* compressed */
        nr = huffman_decompress(&huffman_state,
                                q, NR,
                                p+2, header-2, &buffer[header]);
    }
    t = time_now()-t;
    printf("%d cy, %d us\n", t, t/TIME_MHZ);
}
/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
