/*
 * tmt20.c - ESC/POS raster encoder for the Epson TM-T20III.
 * See tmt20.h and ../../RE_NOTES.md for where every byte comes from.
 */
#include "tmt20.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* A run of this many blank rows inside a band ends the band and is fed
 * instead of being sent as raster. */
#define TM_BLANK_RUN_SPLIT 32

struct tm_encoder {
    tm_options  o;
    tm_write_fn write;
    void       *ctx;
    int         err;

    int         row_bytes;
    uint8_t    *band;          /* band_rows * row_bytes                      */
    int         band_n;        /* rows currently in band                     */
    int         band_trailing; /* blank rows at the end of band              */
    uint8_t    *work;          /* compress / sanitise scratch                */
    size_t      work_cap;

    long        pending_blank; /* blank rows waiting to be fed               */
    int         page_has_ink;

    long        in_rows;       /* rows received this job (for length scale)  */
    long        out_rows;      /* rows produced after scaling                */
    long        emitted_rows;  /* stats: raster + fed rows                   */
};

/* ---------------------------------------------------------------- output */

static void put(tm_encoder *e, const uint8_t *p, size_t n)
{
    if (e->err || n == 0)
        return;
    if (e->write(e->ctx, p, n) != n)
        e->err = -1;
}

static void esc_j(tm_encoder *e, long units)
{
    while (units > 0) {
        uint8_t c[3] = {0x1B, 0x4A, (uint8_t)(units > 255 ? 255 : units)};
        put(e, c, 3);
        units -= c[2];
    }
}

static void cut_cmd(tm_encoder *e, int mode)
{
    static const uint8_t c0[]  = {0x1B, 0x4A, 0x00};
    static const uint8_t c1[]  = {0x1B, 0x4A, 0x00, 0x1D, 0x56, 0x00};
    static const uint8_t c16[] = {0x1B, 0x4A, 0xFF, 0x1B, 0x4A, 0xC1};
    static const uint8_t c17[] = {0x1B, 0x4A, 0x00, 0x1D, 0x56, 0x41, 0x00};
    switch (mode) {
    case TM_CUT_NOFEED_CUT: put(e, c1, sizeof c1); break;
    case TM_CUT_FEED_NOCUT: put(e, c16, sizeof c16); break;
    case TM_CUT_FEED_CUT:   put(e, c17, sizeof c17); break;
    default:                put(e, c0, sizeof c0); break;
    }
}

/* PrinterSpec command_dk_<mode> (TM-T20III.ini [DRV_PRINTER_SPEC]). */
static void dk_cmd(tm_encoder *e, int mode, int count)
{
    static const uint8_t d1[]   = {0x1B, 0x70, 0x30, 0x32, 0xC8};
    static const uint8_t d2[]   = {0x1B, 0x70, 0x31, 0x32, 0xC8};
    static const uint8_t b256[] = {0x1B, 0x70, 0x31, 0xFA, 0x64};
    static const uint8_t b257[] = {0x1B, 0x70, 0x31, 0xFA, 0x64, 0x1B, 0x70, 0x30, 0x0A, 0x64};
    static const uint8_t b258[] = {0x1B, 0x70, 0x30, 0xFA, 0x64, 0x1B, 0x70, 0x31, 0x0A, 0x64};
    switch (mode) {
    case TM_DK_DRAWER1:        put(e, d1, sizeof d1); break;
    case TM_DK_DRAWER2:        put(e, d2, sizeof d2); break;
    case TM_DK_DRAWER12:       put(e, d1, sizeof d1); put(e, d2, sizeof d2); break;
    case TM_DK_BUZZER:         put(e, b256, sizeof b256); break;
    case TM_DK_BUZZER_DRAWER1: put(e, b257, sizeof b257); break;
    case TM_DK_BUZZER_DRAWER2: put(e, b258, sizeof b258); break;
    default:
        if (mode >= TM_DK_EXT_BUZZER_A && mode <= TM_DK_EXT_BUZZER_E) {
            int m = count < 1 ? 1 : count > 255 ? 255 : count;
            uint8_t b[8] = {0x1B, 0x28, 0x41, 0x03, 0x00, 0x61,
                            (uint8_t)(mode - TM_DK_EXT_BUZZER_A + 1), (uint8_t)m};
            put(e, b, sizeof b);
        }
        break;
    }
}

static long um_to_units(const tm_encoder *e, int um)
{
    double per_inch = (double)e->o.dpi_y * e->o.y_units_per_row;  /* 406 */
    return um <= 0 ? 0 : lround(um * per_inch / 25400.0);
}

/* EA6RDPTM DoOperation: (layout cmd) -> user command -> drawer/buzzer ->
 * ESC J FeedExtra -> (logo) -> cut. */
static void do_operation(tm_encoder *e, int at)
{
    const tm_op *op = &e->o.op[at];
    if (op->cmd_len)
        put(e, op->cmd, op->cmd_len > TM_CMD_MAX ? TM_CMD_MAX : op->cmd_len);
    dk_cmd(e, op->dk, op->dk_count);
    esc_j(e, um_to_units(e, op->feed_um));
    cut_cmd(e, op->cut);
}

/* --------------------------------------------------- real-time protection */

/* Port of EA6RDPTM FUN_180057bf0: does (compressed) data contain a DLE DC4
 * real-time command the printer would execute mid-image? */
int tm_has_realtime_dc4(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        size_t rem = n - i;
        if (b[i] != 0x10 || rem <= 4 || b[i + 1] != 0x14)
            continue;
        uint8_t fn = b[i + 2];
        if (fn == 1 && b[i + 3] < 2 && (uint8_t)(b[i + 4] - 1) < 8)
            return 1;                                   /* pulse        */
        if (fn == 2 && b[i + 3] == 1 && b[i + 4] == 8)
            return 1;                                   /* power off    */
        if (rem > 7 && fn == 3 && b[i + 3] < 9 && b[i + 4] < 9 &&
            b[i + 5] < 9 && (uint8_t)(b[i + 6] - 1) < 8 && b[i + 7] < 9)
            return 1;                                   /* buzzer       */
        if (rem > 9 && fn == 8 && b[i + 3] == 1 && b[i + 4] == 3 &&
            b[i + 5] == 0x14 && b[i + 6] == 1 && b[i + 7] == 6 &&
            b[i + 8] == 2 && b[i + 9] == 8)
            return 1;                                   /* clear buffer */
    }
    return 0;
}

/* Port of EA6RDPTM FUN_180057ba0: neuter DLE EOT/ENQ/DC4 and ESC = in raw
 * pixel data by replacing the lead byte with 0x30 (adds one stray dot). */
void tm_sanitize_raw(uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        size_t rem = n - i;
        if (rem <= 2)
            break;
        if (b[i] == 0x10) {
            uint8_t c = b[i + 1];
            if (c == 0x04 || c == 0x05 || c == 0x14)
                b[i] = 0x30;
        } else if (b[i] == 0x1B && b[i + 1] == 0x3D) {
            b[i] = 0x30;
        }
    }
}

/* ------------------------------------------------------------- graphics */

static int ensure_work(tm_encoder *e, size_t need)
{
    if (need <= e->work_cap)
        return 0;
    uint8_t *p = realloc(e->work, need);
    if (!p) {
        e->err = -1;
        return -1;
    }
    e->work = p;
    e->work_cap = need;
    return 0;
}

/* Send nrows of row data as one GS 8 L store + GS ( L print. */
static void emit_graphics(tm_encoder *e, const uint8_t *rows, int nrows)
{
    if (nrows <= 0 || e->err)
        return;
    size_t raw = (size_t)nrows * e->row_bytes;
    uint8_t a = 0x30;
    size_t len = raw;

    if (e->o.compress) {
        uLongf zlen = compressBound((uLong)raw);
        if (ensure_work(e, zlen) == 0 &&
            compress2(e->work, &zlen, rows, (uLong)raw, Z_DEFAULT_COMPRESSION) == Z_OK &&
            !tm_has_realtime_dc4(e->work, zlen)) {
            a = 0x31;
            len = zlen;
        }
    }
    if (a == 0x30) {
        if (ensure_work(e, raw) != 0)
            return;
        memcpy(e->work, rows, raw);
        tm_sanitize_raw(e->work, raw);
    }

    uint32_t p = (uint32_t)len + 10;
    uint8_t hdr[17] = {
        0x1D, 0x38, 0x4C,
        (uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16), (uint8_t)(p >> 24),
        0x30, 0x70, a, 0x01, 0x01, 0x31,
        (uint8_t)e->o.width_dots, (uint8_t)(e->o.width_dots >> 8),
        (uint8_t)nrows, (uint8_t)(nrows >> 8)
    };
    static const uint8_t print[] = {0x1D, 0x28, 0x4C, 0x02, 0x00, 0x30, 0x32};
    put(e, hdr, sizeof hdr);
    put(e, e->work, len);
    put(e, print, sizeof print);
    e->emitted_rows += nrows;
}

static void feed_rows(tm_encoder *e, long rows)
{
    if (rows <= 0)
        return;
    if (e->o.blank_mode == TM_BLANK_RASTER) {
        /* Reuse the (currently empty) band buffer as a source of zeros. */
        memset(e->band, 0, (size_t)e->o.band_rows * e->row_bytes);
        while (rows > 0 && !e->err) {
            int n = rows > e->o.band_rows ? e->o.band_rows : (int)rows;
            emit_graphics(e, e->band, n);
            rows -= n;
        }
    } else {
        esc_j(e, rows * e->o.y_units_per_row);
        e->emitted_rows += rows;
    }
}

/* Flush band rows [0, n); remaining rows (if any) are discarded. */
static void flush_band(tm_encoder *e, int n)
{
    emit_graphics(e, e->band, n);
    e->band_n = 0;
    e->band_trailing = 0;
}

static int row_is_blank(const uint8_t *r, int nbytes)
{
    for (int i = 0; i < nbytes; i++)
        if (r[i])
            return 0;
    return 1;
}

static void push_row(tm_encoder *e, const uint8_t *row)
{
    if (row_is_blank(row, e->row_bytes)) {
        if (e->band_n == 0) {
            if (!e->page_has_ink && (e->o.trim & TM_TRIM_TOP))
                return;                             /* top trim */
            e->pending_blank++;
            return;
        }
        memset(e->band + (size_t)e->band_n * e->row_bytes, 0, e->row_bytes);
        e->band_n++;
        e->band_trailing++;
        if (e->band_trailing >= TM_BLANK_RUN_SPLIT) {
            int t = e->band_trailing;
            flush_band(e, e->band_n - t);
            e->pending_blank += t;
        } else if (e->band_n == e->o.band_rows) {
            int t = e->band_trailing;
            flush_band(e, e->band_n - t);
            e->pending_blank += t;
        }
        return;
    }

    /* Ink row. */
    if (e->pending_blank) {
        feed_rows(e, e->pending_blank);
        e->pending_blank = 0;
    }
    memcpy(e->band + (size_t)e->band_n * e->row_bytes, row, e->row_bytes);
    e->band_n++;
    e->band_trailing = 0;
    e->page_has_ink = 1;
    if (e->band_n == e->o.band_rows)
        flush_band(e, e->band_n);
}

/* ------------------------------------------------------------------ API */

void tm_default_options(tm_options *o)
{
    memset(o, 0, sizeof *o);
    o->width_dots      = 576;
    o->dpi_y           = 203;
    o->y_units_per_row = 2;
    o->band_rows       = 256;
    o->compress        = 1;
    o->length_scale    = 1.0;
    o->trim            = TM_TRIM_NONE;
    o->blank_mode      = TM_BLANK_FEED;
    tm_apply_feedcut_preset(o, TM_PRESET_DOC_FEED_CUT);   /* Windows default */
    for (int i = 0; i < TM_AT_COUNT; i++)
        o->op[i].dk_count = 1;
}

void tm_apply_feedcut_preset(tm_options *o, tm_feedcut_preset p)
{
    static const int modes[TM_PRESET_COUNT][TM_AT_COUNT] = {
        /* DocBegin PageBegin PageEnd DocEnd */
        {0, 0,  0, 17},   /* Document[Feed, Cut]       */
        {0, 0,  0, 16},   /* Document[Feed, No cut]    */
        {0, 0,  0,  1},   /* Document[No feed, Cut]    */
        {0, 0,  0,  0},   /* Document[No feed, No cut] */
        {0, 0, 17,  0},   /* Page[Feed, Cut]           */
        {0, 0, 16,  0},   /* Page[Feed, No cut]        */
        {0, 0,  1,  0},   /* Page[No feed, Cut]        */
    };
    if ((int)p < 0 || p >= TM_PRESET_COUNT)
        return;
    for (int i = 0; i < TM_AT_COUNT; i++) {
        o->op[i].cut = modes[p][i];
        o->op[i].feed_um = 0;
    }
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int tm_parse_cmd(const char *t, uint8_t *out, size_t cap)
{
    size_t n = 0;
    if (!t)
        return 0;
    while (*t) {
        if (*t == '"') {                                  /* "literal text" */
            for (t++; *t && *t != '"'; t++) {
                if (n == cap)
                    return -1;
                out[n++] = (uint8_t)*t;
            }
            if (*t != '"')
                return -1;
            t++;
        } else if (*t == ' ' || *t == ',' || *t == '\t' || *t == '<' || *t == '>') {
            t++;
        } else if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
            t += 2;
        } else {
            int hi = hexval((unsigned char)t[0]);
            int lo = hi < 0 ? -1 : hexval((unsigned char)t[1]);
            if (hi < 0 || lo < 0 || n == cap)
                return -1;
            out[n++] = (uint8_t)(hi << 4 | lo);
            t += 2;
        }
    }
    return (int)n;
}

tm_encoder *tm_new(const tm_options *o, tm_write_fn write, void *ctx)
{
    tm_encoder *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->o = *o;
    if (e->o.width_dots <= 0 || e->o.width_dots > 65535) e->o.width_dots = 576;
    if (e->o.band_rows <= 0 || e->o.band_rows > 2048)    e->o.band_rows = 256;
    if (e->o.y_units_per_row <= 0)                       e->o.y_units_per_row = 2;
    if (e->o.dpi_y <= 0)                                 e->o.dpi_y = 203;
    if (!(e->o.length_scale > 0.05 && e->o.length_scale < 4.0)) e->o.length_scale = 1.0;
    e->write = write;
    e->ctx = ctx;
    e->row_bytes = (e->o.width_dots + 7) / 8;
    e->band = calloc((size_t)e->o.band_rows, (size_t)e->row_bytes);
    if (!e->band) {
        free(e);
        return NULL;
    }
    return e;
}

void tm_free(tm_encoder *e)
{
    if (!e)
        return;
    free(e->band);
    free(e->work);
    free(e);
}

int tm_row_bytes(const tm_encoder *e) { return e->row_bytes; }
long tm_rows_emitted(const tm_encoder *e) { return e->emitted_rows; }

int tm_job_begin(tm_encoder *e)
{
    /* GPD JobSetup1 (ANK): ESC = 1; GS ( J x3; ESC c 0 2; ESC c 1 2;
     * ESC c 3 0; ESC 2; ESC R 0; ESC t 16; GS b 1 */
    static const uint8_t setup[] = {
        0x1B, 0x3D, 0x01,
        0x1D, 0x28, 0x4A, 0x02, 0x00, 0x01, 0x00,
        0x1D, 0x28, 0x4A, 0x02, 0x00, 0x02, 0x00,
        0x1D, 0x28, 0x4A, 0x02, 0x00, 0x03, 0x00,
        0x1B, 0x63, 0x30, 0x02,
        0x1B, 0x63, 0x31, 0x02,
        0x1B, 0x63, 0x33, 0x00,
        0x1B, 0x32,
        0x1B, 0x52, 0x00,
        0x1B, 0x74, 0x10,
        0x1D, 0x62, 0x01
    };
    put(e, setup, sizeof setup);
    do_operation(e, TM_AT_DOC_BEGIN);
    if (e->o.print_control >= 1 && e->o.print_control <= 3) {
        uint8_t c[7] = {0x1D, 0x28, 0x4B, 0x02, 0x00, 0x30,
                        (uint8_t)(0x30 + e->o.print_control)};
        put(e, c, sizeof c);
    }
    e->in_rows = e->out_rows = 0;
    return e->err;
}

int tm_page_begin(tm_encoder *e)
{
    static const uint8_t page_setup[] = {0x1B, 0x4A, 0x00};
    put(e, page_setup, sizeof page_setup);
    do_operation(e, TM_AT_PAGE_BEGIN);
    esc_j(e, um_to_units(e, e->o.top_margin_um));     /* Adjust Print Position */
    e->band_n = 0;
    e->band_trailing = 0;
    e->pending_blank = 0;
    e->page_has_ink = 0;
    return e->err;
}

int tm_page_row(tm_encoder *e, const uint8_t *row)
{
    /* Length calibration: nearest-neighbour vertical resample, continuous
     * across pages so a multi-page tape scales as one strip. */
    e->in_rows++;
    long target = lround((double)e->in_rows * e->o.length_scale);
    long copies = target - e->out_rows;
    e->out_rows = target;
    for (long i = 0; i < copies && !e->err; i++)
        push_row(e, row);
    return e->err;
}

int tm_page_end(tm_encoder *e)
{
    if (e->band_n) {
        int t = e->band_trailing;
        flush_band(e, e->band_n - t);
        e->pending_blank += t;
    }
    if (!(e->o.trim & TM_TRIM_BOTTOM))
        feed_rows(e, e->pending_blank);
    e->pending_blank = 0;

    static const uint8_t page_finish[] = {0x1B, 0x4A, 0x00};
    put(e, page_finish, sizeof page_finish);
    do_operation(e, TM_AT_PAGE_END);
    return e->err;
}

int tm_job_end(tm_encoder *e)
{
    do_operation(e, TM_AT_DOC_END);
    return e->err;
}
