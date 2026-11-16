/*
 * tmdriver.c - Printing Preferences model <-> queue PPD (see tmdriver.h).
 */
#include "tmdriver.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------ catalog */

#define T4(pfx, def, ch, lo, hi) \
    {pfx "docbegin", TM_DRV_SETTING, "0", ch, lo, hi}, \
    {pfx "pagebegin", TM_DRV_SETTING, "0", ch, lo, hi}, \
    {pfx "pageend", TM_DRV_SETTING, "0", ch, lo, hi}, \
    {pfx "docend", TM_DRV_SETTING, def, ch, lo, hi}

#define DK_CHOICES "0|1|2|3|256|257|258|512|513|514|515|516"

static const tm_drv_desc descs[] = {
    /* print dialog (PPD UI options) */
    {"PageSize",       TM_DRV_UI, "RP80x297", NULL, 1, 0},
    {"ColorModel",     TM_DRV_UI, "Mono", "Mono|Gray", 0, 0},
    {"TmPaperSource",  TM_DRV_UI, "None", "None|Top|Bottom|Both", 0, 0},
    {"Resolution",     TM_DRV_UI, "203dpi", "203dpi", 0, 0},
    {"TmHalftone",     TM_DRV_UI, "Dither8x8", "Dither8x8", 0, 0},
    {"TmPaperWidth",   TM_DRV_UI, "80mm", "80mm|58mm", 0, 0},
    {"TmScaling",      TM_DRV_UI, "100", "100|70", 0, 0},
    {"TmUpsideDown",   TM_DRV_UI, "False", "False|True", 0, 0},
    {"TmFeedCut",      TM_DRV_UI, "DocFeedCut",
     "DocFeedCut|DocFeedNoCut|DocNoFeedCut|DocNoFeedNoCut|PageFeedCut|PageFeedNoCut|PageNoFeedCut|Detail", 0, 0},
    {"TmPeripherals",  TM_DRV_UI, "Default", "Default|None", 0, 0},
    {"TmLengthScale",  TM_DRV_UI, "1.000", "0.990|0.995|1.000|1.005|1.010|1.015|1.020", 0, 0},
    {"TmBlankFeed",    TM_DRV_UI, "Feed", "Feed|Raster", 0, 0},
    {"TmCompress",     TM_DRV_UI, "True", "True|False", 0, 0},
    {"TmPrintControl", TM_DRV_UI, "Default", "Default|Mode1|Mode2|Mode3", 0, 0},
    /* Feed and Cut detail (cutter.<timing>.Mode / FeedExtra / FeedToStartPos) */
    T4("tm-cut-", "17", "0|1|16|17", 0, 0),
    T4("tm-feed-", "0", NULL, 0, 1000000),
    T4("tm-startpos-", "0", "0|1", 0, 0),
    {"tm-feed-unit",   TM_DRV_SETTING, "mm", "mm|inch", 0, 0},
    /* Peripherals (dk.<timing>.Mode / Cycle) */
    T4("tm-dk-", "0", DK_CHOICES, 0, 0),
    {"tm-dk-count-docbegin",  TM_DRV_SETTING, "1", NULL, 1, 255},
    {"tm-dk-count-pagebegin", TM_DRV_SETTING, "1", NULL, 1, 255},
    {"tm-dk-count-pageend",   TM_DRV_SETTING, "1", NULL, 1, 255},
    {"tm-dk-count-docend",    TM_DRV_SETTING, "1", NULL, 1, 255},
    /* APD Utility: Send Command (command.startdoc / startpage / endpage / enddoc) */
    {"tm-cmd-docbegin",  TM_DRV_SETTING, "", NULL, 1, 0},
    {"tm-cmd-pagebegin", TM_DRV_SETTING, "", NULL, 1, 0},
    {"tm-cmd-pageend",   TM_DRV_SETTING, "", NULL, 1, 0},
    {"tm-cmd-docend",    TM_DRV_SETTING, "", NULL, 1, 0},
    /* APD Utility: Layout (layout.margin.*, reduce, layout.UnsupportedPaper) */
    {"tm-margin-top-enable",  TM_DRV_SETTING, "0", "0|1", 0, 0},
    {"tm-margin-top",         TM_DRV_SETTING, "0", NULL, 0, 11000},
    {"tm-margin-left-enable", TM_DRV_SETTING, "0", "0|1", 0, 0},
    {"tm-margin-left",        TM_DRV_SETTING, "0", NULL, 0, 11000},
    {"tm-reduce-80-58",       TM_DRV_SETTING, "0", "0|1", 0, 0},
    {"tm-unsupported-auto",   TM_DRV_SETTING, "1", "0|1", 0, 0},
};
static const int ndescs = (int)(sizeof descs / sizeof descs[0]);

const tm_drv_desc *tm_drv_desc_at(int i) { return i >= 0 && i < ndescs ? &descs[i] : NULL; }

const tm_drv_desc *tm_drv_desc_find(const char *key)
{
    for (int i = 0; key && i < ndescs; i++)
        if (!strcmp(descs[i].key, key))
            return &descs[i];
    return NULL;
}

const char *tm_drv_strerror(int err)
{
    switch (err) {
    case TM_DRV_OK:        return "ok";
    case TM_DRV_EKEY:      return "unknown setting";
    case TM_DRV_EVALUE:    return "invalid value";
    case TM_DRV_EFULL:     return "too many entries";
    case TM_DRV_ERANGE:    return "paper size out of range (width 52.6-210 mm, height 25.4-3276 mm)";
    case TM_DRV_EMARGIN:   return "margin exceeding the paper size";
    case TM_DRV_ENAME:     return "invalid paper name (1-31 characters, no : / \")";
    case TM_DRV_ENOTFOUND: return "not found";
    default:               return "error";
    }
}

/* -------------------------------------------------------------- values */

static int in_choices(const char *choices, const char *v)
{
    size_t vl = strlen(v);
    for (const char *p = choices; p && *p;) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        if (n == vl && !strncmp(p, v, n))
            return 1;
        p = bar ? bar + 1 : NULL;
    }
    return 0;
}

static int find_kv(const tm_drv *d, const char *key)
{
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->key[i], key))
            return i;
    return -1;
}

void tm_drv_defaults(tm_drv *d)
{
    memset(d, 0, sizeof *d);
}

const char *tm_drv_get(const tm_drv *d, const char *key)
{
    int i = find_kv(d, key);
    if (i >= 0)
        return d->val[i];
    const tm_drv_desc *s = tm_drv_desc_find(key);
    return s ? s->def : NULL;
}

int tm_drv_is_default(const tm_drv *d, const char *key)
{
    const tm_drv_desc *s = tm_drv_desc_find(key);
    const char *v = tm_drv_get(d, key);
    return s && v && !strcmp(v, s->def);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Same grammar as tm_parse_cmd() in the filter. */
int tm_drv_cmd_normalize(const char *t, char *out, size_t cap)
{
    size_t o = 0, nbytes = 0;
    if (cap)
        out[0] = 0;
    while (t && *t) {
        int byte[256], nb = 0;
        if (*t == '"') {
            for (t++; *t && *t != '"'; t++) {
                if (nb == 256) return -1;
                byte[nb++] = (unsigned char)*t;
            }
            if (*t != '"') return -1;
            t++;
        } else if (*t == ' ' || *t == ',' || *t == '\t' || *t == '<' || *t == '>' ||
                   *t == '\r' || *t == '\n') {
            t++;
        } else if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
            t += 2;
        } else {
            int hi = hexval((unsigned char)t[0]);
            int lo = hi < 0 ? -1 : hexval((unsigned char)t[1]);
            if (hi < 0 || lo < 0) return -1;
            byte[nb++] = hi << 4 | lo;
            t += 2;
        }
        for (int i = 0; i < nb; i++) {
            if (++nbytes > 256 || o + 4 > cap) return -1;
            o += (size_t)snprintf(out + o, cap - o, o ? " %02X" : "%02X", byte[i]);
        }
    }
    return 0;
}

static int valid_keyword(const char *v)
{
    if (!*v || strlen(v) > 40)
        return 0;
    for (; *v; v++)
        if (!isalnum((unsigned char)*v) && *v != '.' && *v != '_' && *v != '-')
            return 0;
    return 1;
}

int tm_drv_set(tm_drv *d, const char *key, const char *value)
{
    const tm_drv_desc *s = tm_drv_desc_find(key);
    char buf[TM_DRV_VAL_LEN];
    if (!s)
        return TM_DRV_EKEY;
    if (!value)
        value = s->def;
    if (s->choices) {
        if (!in_choices(s->choices, value))
            return TM_DRV_EVALUE;
    } else if (s->min > s->max) {                       /* free text */
        if (!strncmp(key, "tm-cmd-", 7)) {
            if (tm_drv_cmd_normalize(value, buf, sizeof buf))
                return TM_DRV_EVALUE;
            value = buf;
        } else if (!strcmp(key, "PageSize")) {
            if (!valid_keyword(value) || !strncasecmp(value, "Custom", 6))
                return TM_DRV_EVALUE;
        }
    } else {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end || n < s->min || n > s->max)
            return TM_DRV_EVALUE;
    }
    if (strlen(value) >= TM_DRV_VAL_LEN)
        return TM_DRV_EVALUE;
    int i = find_kv(d, key);
    if (i < 0) {
        if (d->n == TM_DRV_MAX_KV)
            return TM_DRV_EFULL;
        i = d->n++;
        snprintf(d->key[i], TM_DRV_KEY_LEN, "%s", key);
    }
    snprintf(d->val[i], TM_DRV_VAL_LEN, "%s", value);
    return TM_DRV_OK;
}

/* -------------------------------------------------------------- papers */

static int valid_name(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 31)
        return 0;
    for (; *n; n++)
        if ((unsigned char)*n < 0x20 || (unsigned char)*n > 0x7E || *n == ':' || *n == '/' || *n == '"')
            return 0;
    return 1;
}

const tm_drv_paper *tm_drv_paper_at(const tm_drv *d, int i)
{
    return i >= 0 && i < d->npapers ? &d->paper[i] : NULL;
}

int tm_drv_paper_add(tm_drv *d, const tm_drv_paper *p)
{
    if (!valid_name(p->name))
        return TM_DRV_ENAME;
    if (p->width_um < TM_UDP_W_MIN_UM || p->width_um > TM_UDP_W_MAX_UM ||
        p->height_um < TM_UDP_H_MIN_UM || p->height_um > TM_UDP_H_MAX_UM)
        return TM_DRV_ERANGE;
    if (p->left_um < 0 || p->right_um < 0 || p->top_um < 0 || p->bottom_um < 0 ||
        p->left_um + p->right_um >= p->width_um || p->top_um + p->bottom_um >= p->height_um)
        return TM_DRV_EMARGIN;
    int slot = -1;
    for (int i = 0; i < d->npapers; i++)
        if (!strcasecmp(d->paper[i].name, p->name))
            slot = i;
    tm_drv_paper np = *p;
    if (slot >= 0) {                                     /* update, keep keyword */
        memcpy(np.keyword, d->paper[slot].keyword, sizeof np.keyword);
    } else {
        if (d->npapers == TM_DRV_MAX_PAPERS)
            return TM_DRV_EFULL;
        slot = d->npapers++;
        for (int k = 1;; k++) {                          /* first free UDPn */
            char kw[24];
            snprintf(kw, sizeof kw, "UDP%d", k);
            int used = 0;
            for (int i = 0; i < d->npapers - 1; i++)
                used |= !strcmp(d->paper[i].keyword, kw);
            if (!used) {
                memcpy(np.keyword, kw, sizeof kw);
                break;
            }
        }
    }
    d->paper[slot] = np;
    return TM_DRV_OK;
}

int tm_drv_paper_del(tm_drv *d, const char *name)
{
    for (int i = 0; i < d->npapers; i++)
        if (!strcasecmp(d->paper[i].name, name)) {
            if (!strcmp(tm_drv_get(d, "PageSize"), d->paper[i].keyword))
                tm_drv_set(d, "PageSize", NULL);
            memmove(&d->paper[i], &d->paper[i + 1], (size_t)(d->npapers - i - 1) * sizeof d->paper[0]);
            d->npapers--;
            return TM_DRV_OK;
        }
    return TM_DRV_ENOTFOUND;
}

/* ------------------------------------------------------------- parsing */

static const char *line_end(const char *p)
{
    const char *e = strchr(p, '\n');
    return e ? e : p + strlen(p);
}

void tm_drv_parse_ppd(tm_drv *d, const char *t)
{
    tm_drv_defaults(d);
    for (const char *p = t; p && *p;) {
        const char *e = line_end(p);
        char line[1024];
        size_t n = (size_t)(e - p) < sizeof line - 1 ? (size_t)(e - p) : sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        if (n && line[n - 1] == '\r')
            line[--n] = 0;
        p = *e ? e + 1 : e;

        char key[64], val[TM_DRV_VAL_LEN];
        if (sscanf(line, "*Default%63[^:]: %599s", key, val) == 2) {
            const tm_drv_desc *s = tm_drv_desc_find(key);
            if (s && s->kind == TM_DRV_UI)
                tm_drv_set(d, key, val);
        } else if (!strncmp(line, "*TmSetting ", 11)) {
            const char *k = line + 11, *colon = strchr(k, ':');
            const char *q1 = colon ? strchr(colon, '"') : NULL;
            const char *q2 = q1 ? strrchr(q1 + 1, '"') : NULL;
            if (q2 && (size_t)(colon - k) < sizeof key && (size_t)(q2 - q1 - 1) < sizeof val) {
                memcpy(key, k, (size_t)(colon - k));
                key[colon - k] = 0;
                memcpy(val, q1 + 1, (size_t)(q2 - q1 - 1));
                val[q2 - q1 - 1] = 0;
                tm_drv_set(d, key, val);
            }
        } else if (!strncmp(line, "*TmUserPaper ", 13)) {
            tm_drv_paper pp;
            memset(&pp, 0, sizeof pp);
            char kw[24], name[32];
            if (sscanf(line + 13, "%23[^/]/%31[^:]: \"%ld %ld %ld %ld %ld %ld %d\"", kw, name,
                       &pp.width_um, &pp.height_um, &pp.left_um, &pp.top_um, &pp.right_um,
                       &pp.bottom_um, &pp.unit_inch) == 9) {
                memcpy(pp.name, name, sizeof pp.name);
                if (tm_drv_paper_add(d, &pp) == TM_DRV_OK)       /* keep the keyword */
                    snprintf(d->paper[d->npapers - 1].keyword, sizeof pp.keyword, "%s", kw);
            }
        }
    }
    /* A PageSize default pointing at a vanished paper falls back. */
    const char *ps = tm_drv_get(d, "PageSize");
    if (!strncmp(ps, "UDP", 3)) {
        int ok = 0;
        for (int i = 0; i < d->npapers; i++)
            ok |= !strcmp(d->paper[i].keyword, ps);
        if (!ok)
            tm_drv_set(d, "PageSize", NULL);
    }
}

/* ---------------------------------------------------------- generation */

typedef struct { char *s; size_t n, cap; int err; } sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->err)
        return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 65536) + n;
        char *p = realloc(b->s, cap);
        if (!p) { b->err = 1; return; }
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sbuf *b, const char *fmt, ...)
{
    char tmp[1400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        sb_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static double pt(long um) { return um * 72.0 / 25400.0; }

static void paper_lines(sbuf *b, const tm_drv *d, const char *what)
{
    for (int i = 0; i < d->npapers; i++) {
        const tm_drv_paper *p = &d->paper[i];
        double w = pt(p->width_um), h = pt(p->height_um);
        if (!strcmp(what, "PageSize") || !strcmp(what, "PageRegion"))
            sb_printf(b, "*%s %s/%s: \"<</PageSize[%.2f %.2f]/ImagingBBox null>>setpagedevice\"\n",
                      what, p->keyword, p->name, w, h);
        else if (!strcmp(what, "ImageableArea"))
            sb_printf(b, "*ImageableArea %s/%s: \"%.2f %.2f %.2f %.2f\"\n", p->keyword, p->name,
                      pt(p->left_um), pt(p->bottom_um), w - pt(p->right_um), h - pt(p->top_um));
        else if (!strcmp(what, "PaperDimension"))
            sb_printf(b, "*PaperDimension %s/%s: \"%.2f %.2f\"\n", p->keyword, p->name, w, h);
    }
}

static const char *ui_default_for(const tm_drv *d, const char *key)
{
    /* PageRegion / ImageableArea / PaperDimension follow PageSize. */
    if (!strcmp(key, "PageRegion") || !strcmp(key, "ImageableArea") || !strcmp(key, "PaperDimension"))
        return tm_drv_get(d, "PageSize");
    const tm_drv_desc *s = tm_drv_desc_find(key);
    return s && s->kind == TM_DRV_UI ? tm_drv_get(d, key) : NULL;
}

char *tm_drv_generate_ppd(const char *t, const tm_drv *d)
{
    if (!t || !strstr(t, "*%@@TMSETTINGS"))
        return NULL;
    sbuf b = {0};
    for (const char *p = t; *p;) {
        const char *e = line_end(p);
        size_t n = (size_t)(e - p);
        char line[1024];
        size_t ln = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, ln);
        line[ln] = 0;
        if (ln && line[ln - 1] == '\r')
            line[--ln] = 0;
        p = *e ? e + 1 : e;

        char key[64];
        if (!strncmp(line, "*%@@UDP ", 8)) {
            paper_lines(&b, d, line + 8);
            sb_printf(&b, "%s\n", line);                  /* keep the marker */
        } else if (!strcmp(line, "*%@@TMSETTINGS")) {
            sb_printf(&b, "%s\n", line);
            for (int i = 0; i < ndescs; i++)
                if (descs[i].kind == TM_DRV_SETTING && !tm_drv_is_default(d, descs[i].key))
                    sb_printf(&b, "*TmSetting %s: \"%s\"\n", descs[i].key, tm_drv_get(d, descs[i].key));
        } else if (!strcmp(line, "*%@@TMUSERPAPER")) {
            sb_printf(&b, "%s\n", line);
            for (int i = 0; i < d->npapers; i++) {
                const tm_drv_paper *pp = &d->paper[i];
                sb_printf(&b, "*TmUserPaper %s/%s: \"%ld %ld %ld %ld %ld %ld %d\"\n", pp->keyword,
                          pp->name, pp->width_um, pp->height_um, pp->left_um, pp->top_um,
                          pp->right_um, pp->bottom_um, pp->unit_inch);
            }
        } else if (sscanf(line, "*Default%63[^:]:", key) == 1 && ui_default_for(d, key)) {
            sb_printf(&b, "*Default%s: %s\n", key, ui_default_for(d, key));
        } else if (!strncmp(line, "*TmSetting ", 11) || !strncmp(line, "*TmUserPaper ", 13) ||
                   (!strncmp(line, "*PageSize UDP", 13) || !strncmp(line, "*PageRegion UDP", 15) ||
                    !strncmp(line, "*ImageableArea UDP", 18) || !strncmp(line, "*PaperDimension UDP", 19))) {
            /* stale generated line in a re-used queue PPD: drop */
        } else {
            sb_add(&b, line, ln);
            sb_add(&b, "\n", 1);
        }
    }
    if (b.err) {
        free(b.s);
        return NULL;
    }
    return b.s;
}

/* ------------------------------------------------------ Swift helpers */

tm_drv *tm_drv_new(void) { return calloc(1, sizeof(tm_drv)); }
void tm_drv_free(tm_drv *d) { free(d); }

tm_drv *tm_drv_copy(const tm_drv *d)
{
    tm_drv *c = malloc(sizeof *c);
    if (c)
        memcpy(c, d, sizeof *c);
    return c;
}

int tm_drv_paper_count(const tm_drv *d) { return d->npapers; }

const char *tm_drv_paper_name(const tm_drv *d, int i)
{
    return i >= 0 && i < d->npapers ? d->paper[i].name : NULL;
}

const char *tm_drv_paper_keyword(const tm_drv *d, int i)
{
    return i >= 0 && i < d->npapers ? d->paper[i].keyword : NULL;
}

int tm_drv_paper_add_simple(tm_drv *d, const char *name, long w, long h,
                            long l, long t, long r, long b, int unit_inch)
{
    tm_drv_paper p;
    memset(&p, 0, sizeof p);
    if (!name || strlen(name) >= sizeof p.name)
        return TM_DRV_ENAME;
    strcpy(p.name, name);
    p.width_um = w; p.height_um = h;
    p.left_um = l; p.top_um = t; p.right_um = r; p.bottom_um = b;
    p.unit_inch = unit_inch;
    return tm_drv_paper_add(d, &p);
}
