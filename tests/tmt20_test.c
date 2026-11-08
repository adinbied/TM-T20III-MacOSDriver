/*
 * Host self-test for the TM-T20III encoder (no CUPS needed).
 *   build/tmt20_test out.bin    -> writes a 2-page job, 2436 rows total
 * Then: python3 tests/escpos_decode.py out.bin --expect-rows 2436
 */
#include "tmt20.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t wr(void *ctx, const uint8_t *b, size_t n) { return fwrite(b, 1, n, (FILE *)ctx); }

static void unit_tests(void)
{
    /* real-time detector */
    const uint8_t pulse[]  = {0x00, 0x10, 0x14, 0x01, 0x00, 0x03, 0x00};
    const uint8_t poweroff[] = {0x10, 0x14, 0x02, 0x01, 0x08, 0x00};
    const uint8_t clear[]  = {0x10, 0x14, 0x08, 0x01, 0x03, 0x14, 0x01, 0x06, 0x02, 0x08, 0x00};
    const uint8_t benign[] = {0x10, 0x14, 0x01, 0x05, 0x03, 0x00, 0x10, 0x04};
    assert(tm_has_realtime_dc4(pulse, sizeof pulse));
    assert(tm_has_realtime_dc4(poweroff, sizeof poweroff));
    assert(tm_has_realtime_dc4(clear, sizeof clear));
    assert(!tm_has_realtime_dc4(benign, sizeof benign));
    assert(!tm_has_realtime_dc4(pulse, 4));               /* too short */

    /* sanitiser */
    uint8_t s[] = {0x10, 0x04, 0x01, 0x10, 0x15, 0x00, 0x1B, 0x3D, 0x01, 0x10, 0x14, 0x01, 0x10, 0x04};
    tm_sanitize_raw(s, sizeof s);
    assert(s[0] == 0x30);                                 /* DLE EOT neutered   */
    assert(s[3] == 0x10);                                 /* DLE NAK untouched  */
    assert(s[6] == 0x30);                                 /* ESC = neutered     */
    assert(s[9] == 0x30);                                 /* DLE DC4 neutered   */
    assert(s[12] == 0x10);                                /* last 2 bytes kept  */
    puts("unit tests: ok");
}

static void make_row(uint8_t *r, int nb, int page, int y)
{
    static uint32_t lcg = 12345;
    memset(r, 0, (size_t)nb);
    if (page == 1) {
        int ink = (y >= 100 && y < 600) || (y >= 1100);
        if (!ink) return;
        for (int x = 0; x < nb; x += 6) r[x] = 0x80;     /* vertical rulings */
        if (y % 50 == 0) memset(r, 0xFF, (size_t)nb);     /* horizontal lines */
    } else {
        if (y >= 1000) return;                            /* trailing blank   */
        for (int i = 0; i < nb; i++) {
            lcg = lcg * 1103515245u + 12345u;
            r[i] = (uint8_t)(lcg >> 16);
        }
        if (y % 7 == 0) { r[10] = 0x10; r[11] = 0x04; }   /* would-be DLE EOT */
        if (y % 11 == 0) { r[20] = 0x10; r[21] = 0x14; r[22] = 0x01; r[23] = 0x00; r[24] = 0x02; }
    }
}

static size_t sink(void *ctx, const uint8_t *b, size_t n) { (void)ctx; (void)b; return n; }

/* Rows emitted for the 2-page test image under given options. */
static long run_rows(tm_options o)
{
    tm_encoder *e = tm_new(&o, sink, NULL);
    uint8_t row[72];
    const int heights[2] = {1200, 1236};
    tm_job_begin(e);
    for (int p = 1; p <= 2; p++) {
        tm_page_begin(e);
        for (int y = 0; y < heights[p - 1]; y++) {
            make_row(row, 72, p, y);
            tm_page_row(e, row);
        }
        tm_page_end(e);
    }
    tm_job_end(e);
    long n = tm_rows_emitted(e);
    tm_free(e);
    return n;
}

static void option_tests(void)
{
    tm_options o;
    tm_default_options(&o);
    long base = run_rows(o);
    assert(base == 2436);

    o.length_scale = 1.015;                     /* calibration */
    long s = run_rows(o);
    printf("scale 1.015: %ld rows (expect %ld)\n", s, (long)(2436 * 1.015 + 0.5));
    assert(s == (long)(2436 * 1.015 + 0.5));

    tm_default_options(&o);
    o.trim = TM_TRIM_BOTTOM;                    /* Epson default: drops page-2 tail */
    assert(run_rows(o) == 2436 - 236);
    o.trim = TM_TRIM_BOTH;                      /* + page-1 leading 100 rows */
    assert(run_rows(o) == 2436 - 236 - 100);

    tm_default_options(&o);
    o.blank_mode = TM_BLANK_RASTER;             /* same length, no ESC J */
    assert(run_rows(o) == 2436);
    puts("option tests: ok");
}

/* ---- per-timing operations (Feed and Cut / Peripherals / Send Command) */

typedef struct { uint8_t b[65536]; size_t n; } membuf;
static size_t memw(void *ctx, const uint8_t *b, size_t n)
{
    membuf *m = ctx;
    if (m->n + n > sizeof m->b) return 0;
    memcpy(m->b + m->n, b, n);
    m->n += n;
    return n;
}

static const uint8_t *find(const membuf *m, const uint8_t *pat, size_t pn, size_t from)
{
    for (size_t i = from; i + pn <= m->n; i++)
        if (!memcmp(m->b + i, pat, pn)) return m->b + i;
    return NULL;
}

static void one_page_job(const tm_options *o, membuf *m)
{
    m->n = 0;
    tm_encoder *e = tm_new(o, memw, m);
    uint8_t row[72];
    memset(row, 0xAA, sizeof row);
    tm_job_begin(e);
    tm_page_begin(e);
    for (int y = 0; y < 10; y++) tm_page_row(e, row);
    tm_page_end(e);
    tm_job_end(e);
    assert(tm_rows_emitted(e) >= 10);
    tm_free(e);
}

static void ops_tests(void)
{
    static membuf m;
    tm_options o;
    uint8_t c[16];

    /* Send Command parser */
    assert(tm_parse_cmd("1B 40", c, sizeof c) == 2 && c[0] == 0x1B && c[1] == 0x40);
    assert(tm_parse_cmd("1b40,0x0A", c, sizeof c) == 3 && c[2] == 0x0A);
    assert(tm_parse_cmd("<1B>\"@A\"", c, sizeof c) == 3 && c[1] == '@' && c[2] == 'A');
    assert(tm_parse_cmd("1B 4", c, sizeof c) == -1);
    assert(tm_parse_cmd("zz", c, sizeof c) == -1);
    assert(tm_parse_cmd("\"unterminated", c, sizeof c) == -1);
    assert(tm_parse_cmd("01 02 03", c, 2) == -1);

    /* Default = Document[Feed, Cut]: job ends with GS V A 0, no cut before. */
    static const uint8_t cut17[] = {0x1B, 0x4A, 0x00, 0x1D, 0x56, 0x41, 0x00};
    static const uint8_t gsv[] = {0x1D, 0x56};
    tm_default_options(&o);
    one_page_job(&o, &m);
    assert(m.n >= sizeof cut17 && !memcmp(m.b + m.n - sizeof cut17, cut17, sizeof cut17));
    assert(find(&m, gsv, 2, 0) == m.b + m.n - 4);              /* only one cut */

    /* Page[No feed, Cut]: GS V 0 after the page, nothing at document end. */
    tm_apply_feedcut_preset(&o, TM_PRESET_PAGE_NOFEED_CUT);
    one_page_job(&o, &m);
    static const uint8_t cut1[] = {0x1B, 0x4A, 0x00, 0x1D, 0x56, 0x00, 0x1B, 0x4A, 0x00};
    assert(!memcmp(m.b + m.n - sizeof cut1, cut1, sizeof cut1));

    /* Custom End of Document: command, drawer 1, external buzzer B x3,
     * 10 mm feed, feed+cut - in DoOperation order. */
    tm_default_options(&o);
    o.op[TM_AT_DOC_END].cmd_len = (size_t)tm_parse_cmd("1B 40", o.op[TM_AT_DOC_END].cmd, TM_CMD_MAX);
    o.op[TM_AT_DOC_END].dk = TM_DK_DRAWER1;
    o.op[TM_AT_DOC_END].feed_um = 10000;                       /* 10 mm = 160 units */
    o.op[TM_AT_PAGE_BEGIN].dk = TM_DK_EXT_BUZZER_A + 1;
    o.op[TM_AT_PAGE_BEGIN].dk_count = 3;
    one_page_job(&o, &m);
    static const uint8_t tail[] = {0x1B, 0x40, 0x1B, 0x70, 0x30, 0x32, 0xC8, 0x1B, 0x4A, 0xA0,
                                   0x1B, 0x4A, 0x00, 0x1D, 0x56, 0x41, 0x00};
    assert(!memcmp(m.b + m.n - sizeof tail, tail, sizeof tail));
    static const uint8_t buzz[] = {0x1B, 0x28, 0x41, 0x03, 0x00, 0x61, 0x02, 0x03};
    const uint8_t *bz = find(&m, buzz, sizeof buzz, 0);
    static const uint8_t store[] = {0x1D, 0x38, 0x4C};
    assert(bz && bz < find(&m, store, 3, 0));                  /* before the raster */

    /* Built-in buzzer + drawer 2 (PrinterSpec command_dk_258) */
    tm_default_options(&o);
    o.op[TM_AT_DOC_BEGIN].dk = TM_DK_BUZZER_DRAWER2;
    one_page_job(&o, &m);
    static const uint8_t dk258[] = {0x1B, 0x70, 0x30, 0xFA, 0x64, 0x1B, 0x70, 0x31, 0x0A, 0x64};
    assert(find(&m, dk258, sizeof dk258, 0));

    /* Top margin 5 mm = 80 ESC J units = 40 rows before the first raster */
    tm_default_options(&o);
    o.top_margin_um = 5000;
    one_page_job(&o, &m);
    static const uint8_t j80[] = {0x1B, 0x4A, 0x50, 0x1D, 0x38, 0x4C};
    assert(find(&m, j80, sizeof j80, 0));
    puts("operation tests: ok");
}

int main(int argc, char **argv)
{
    unit_tests();
    option_tests();
    ops_tests();
    FILE *f = fopen(argc > 1 ? argv[1] : "test_out.bin", "wb");
    if (!f) { perror("open"); return 1; }

    tm_options o;
    tm_default_options(&o);
    tm_encoder *e = tm_new(&o, wr, f);
    uint8_t row[72];
    assert(tm_row_bytes(e) == 72);

    /* Expected paper image, for a pixel-exact round trip check. */
    char epath[512];
    snprintf(epath, sizeof epath, "%s.expected.pbm", argc > 1 ? argv[1] : "test_out.bin");
    FILE *ef = fopen(epath, "wb");
    if (ef) fprintf(ef, "P4\n576 2436\n");

    tm_job_begin(e);
    const int heights[2] = {1200, 1236};
    for (int p = 1; p <= 2; p++) {
        tm_page_begin(e);
        for (int y = 0; y < heights[p - 1]; y++) {
            make_row(row, 72, p, y);
            if (ef) fwrite(row, 1, 72, ef);
            tm_page_row(e, row);
        }
        tm_page_end(e);
    }
    if (ef) fclose(ef);
    tm_job_end(e);
    printf("rows emitted: %ld (expected 2436)\n", tm_rows_emitted(e));
    int ok = tm_rows_emitted(e) == 2436;
    tm_free(e);
    fclose(f);
    return ok ? 0 : 1;
}
