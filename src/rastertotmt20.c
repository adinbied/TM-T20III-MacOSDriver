/*
 * rastertotmt20 - CUPS raster filter for the Epson TM-T20III.
 *
 * usage (called by cupsd): rastertotmt20 job user title copies options [file]
 *
 * Input : application/vnd.cups-raster, 203 dpi, K or W colour space,
 *         1-bit (system dither) or 8-bit (filter applies 8x8 Bayer dither).
 * Output: ESC/POS on stdout (see tmt20.c).
 *
 * Settings mirror the Windows APD6 driver.  Each is looked up, in order, as
 *   1. a job option (lp -o name=value),
 *   2. a PPD UI option (print dialog / queue default),
 *   3. a "*TmSetting name: "value"" line in the queue's PPD, written by
 *      TMUtility.app's Printing Preferences (= Windows DEVMODE/printer data),
 *   4. the Windows default.
 *
 * PPD UI options: TmFeedCut, TmPaperSource, TmPaperWidth, TmScaling,
 *   TmUpsideDown, TmPeripherals, TmLengthScale, TmBlankFeed, TmCompress,
 *   TmPrintControl (+ legacy TmTrim).
 * Detail settings (Printing Preferences / APD Utility):
 *   tm-cut-<t>, tm-feed-<t>, tm-dk-<t>, tm-dk-count-<t>, tm-cmd-<t>
 *     with <t> = docbegin | pagebegin | pageend | docend
 *   tm-margin-top, tm-margin-left (micrometres), tm-margin-top-enable,
 *   tm-margin-left-enable, tm-reduce-80-58, tm-unsupported-auto.
 */
#include "tmt20.h"

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_cancel = 0;
static void on_term(int sig) { (void)sig; g_cancel = 1; }

static size_t write_stdout(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(1, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return off;
        }
        off += (size_t)n;
    }
    return off;
}

/* ------------------------------------------------------------ options */

typedef struct {
    int            num;
    cups_option_t *opts;
    ppd_file_t    *ppd;
} opt_ctx;

static const char *opt(const opt_ctx *c, const char *name)
{
    const char *v = cupsGetOption(name, c->num, c->opts);
    if (v)
        return v;
    if (c->ppd) {
        ppd_choice_t *ch = ppdFindMarkedChoice(c->ppd, name);
        if (ch)
            return ch->choice;
        ppd_attr_t *a = ppdFindAttr(c->ppd, "TmSetting", name);
        if (a && a->value)
            return a->value;
    }
    return NULL;
}

static int opt_is(const char *v, const char *s) { return v && !strcasecmp(v, s); }

static long opt_long(const opt_ctx *c, const char *name, long def)
{
    const char *v = opt(c, name);
    if (!v || !*v)
        return def;
    if (opt_is(v, "true") || opt_is(v, "yes") || opt_is(v, "on"))
        return 1;
    if (opt_is(v, "false") || opt_is(v, "no") || opt_is(v, "off"))
        return 0;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    return end == v ? def : n;
}

/* "1.015", "Custom.1.015", "1015" (per-mille) all accepted. */
static double parse_scale(const char *v)
{
    if (!v) return 1.0;
    if (!strncasecmp(v, "Custom.", 7)) v += 7;
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v) return 1.0;
    if (d > 100.0) d /= 1000.0;                 /* per-mille form */
    return (d > 0.5 && d < 2.0) ? d : 1.0;
}

static const char *const timing_names[TM_AT_COUNT] = {"docbegin", "pagebegin", "pageend", "docend"};

static const char *const preset_names[TM_PRESET_COUNT] = {
    "DocFeedCut", "DocFeedNoCut", "DocNoFeedCut", "DocNoFeedNoCut",
    "PageFeedCut", "PageFeedNoCut", "PageNoFeedCut"
};

/* Filter-side settings that act on the image, not the byte stream. */
typedef struct {
    int    head_dots;          /* 576 (80 mm) / 420 (58 mm)                  */
    double page_scale;         /* Reduced size print / 80->58 / auto-fit     */
    int    reduce_80_58;
    int    unsupported_auto;
    int    left_dots;
    int    upside_down;
} img_opts;

static void load_options(const opt_ctx *c, tm_options *o, img_opts *im)
{
    const char *v;
    char key[64];

    /* Feed and Cut: preset, or the per-timing detail settings. */
    v = opt(c, "TmFeedCut");
    int preset = -1;
    for (int i = 0; v && i < TM_PRESET_COUNT; i++)
        if (opt_is(v, preset_names[i]))
            preset = i;
    if (preset >= 0) {
        tm_apply_feedcut_preset(o, (tm_feedcut_preset)preset);
    } else {                                    /* Detail (shown as "Custom") */
        for (int t = 0; t < TM_AT_COUNT; t++) {
            snprintf(key, sizeof key, "tm-cut-%s", timing_names[t]);
            o->op[t].cut = (int)opt_long(c, key, o->op[t].cut);
            snprintf(key, sizeof key, "tm-feed-%s", timing_names[t]);
            o->op[t].feed_um = (int)opt_long(c, key, 0);
        }
    }

    /* Peripherals + Send Command (independent of the preset, as on Windows). */
    int periph = !opt_is(opt(c, "TmPeripherals"), "None");
    for (int t = 0; t < TM_AT_COUNT; t++) {
        snprintf(key, sizeof key, "tm-dk-%s", timing_names[t]);
        o->op[t].dk = periph ? (int)opt_long(c, key, 0) : 0;
        snprintf(key, sizeof key, "tm-dk-count-%s", timing_names[t]);
        o->op[t].dk_count = (int)opt_long(c, key, 1);
        snprintf(key, sizeof key, "tm-cmd-%s", timing_names[t]);
        v = opt(c, key);
        if (v && *v) {
            int n = tm_parse_cmd(v, o->op[t].cmd, TM_CMD_MAX);
            if (n < 0)
                fprintf(stderr, "WARNING: ignoring invalid %s\n", key);
            o->op[t].cmd_len = n > 0 ? (size_t)n : 0;
        }
    }

    /* Paper/Quality "Paper Source" = Epson paper conservation. */
    v = opt(c, "TmPaperSource");
    if (!v)
        v = opt(c, "TmTrim");                   /* legacy name */
    o->trim = opt_is(v, "Top") ? TM_TRIM_TOP : opt_is(v, "Bottom") ? TM_TRIM_BOTTOM :
              opt_is(v, "Both") ? TM_TRIM_BOTH : TM_TRIM_NONE;

    v = opt(c, "TmBlankFeed");
    o->blank_mode = opt_is(v, "Raster") ? TM_BLANK_RASTER : TM_BLANK_FEED;

    o->length_scale = parse_scale(opt(c, "TmLengthScale"));

    v = opt(c, "TmCompress");
    o->compress = !(opt_is(v, "False") || opt_is(v, "No") || opt_is(v, "Off"));

    v = opt(c, "TmPrintControl");               /* Default | Mode1..3 */
    o->print_control = (v && !strncasecmp(v, "Mode", 4)) ? atoi(v + 4) : 0;

    /* Layout (APD Utility "Adjust Print Position"). */
    if (opt_long(c, "tm-margin-top-enable", 0))
        o->top_margin_um = (int)opt_long(c, "tm-margin-top", 0);
    im->left_dots = 0;
    if (opt_long(c, "tm-margin-left-enable", 0))
        im->left_dots = (int)lround(opt_long(c, "tm-margin-left", 0) * 203.0 / 25400.0);

    v = opt(c, "TmPaperWidth");
    im->head_dots = opt_is(v, "58mm") ? 420 : 576;

    v = opt(c, "TmScaling");                    /* Reduced size print 100% / 70% */
    im->page_scale = opt_is(v, "70") ? 0.70 : 1.0;
    im->reduce_80_58 = (int)opt_long(c, "tm-reduce-80-58", 0);
    im->unsupported_auto = (int)opt_long(c, "tm-unsupported-auto", 1);
    im->upside_down = opt_is(opt(c, "TmUpsideDown"), "True");
}

/* ------------------------------------------------------- pixel convert */

static const uint8_t bayer8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}
};

/* Head dot x -> source pixel x (or -1).  Built once per page. */
static void build_xmap(int *xmap, int head, int left, int xoff, int src_w, double scale)
{
    for (int x = 0; x < head; x++) {
        int sx = -1;
        if (x >= left) {
            sx = xoff + (int)floor((x - left) / scale);
            if (sx >= src_w)
                sx = -1;
        }
        xmap[x] = sx;
    }
}

static void convert_row(const cups_page_header2_t *h, const uint8_t *in,
                        uint8_t *out, int width, const int *xmap, int y)
{
    int white_is_one = (h->cupsColorSpace == CUPS_CSPACE_W ||
                        h->cupsColorSpace == CUPS_CSPACE_SW);
    memset(out, 0, (size_t)(width + 7) / 8);
    for (int x = 0; x < width; x++) {
        int sx = xmap[x];
        if (sx < 0)
            continue;
        int black;
        if (h->cupsBitsPerColor == 1) {
            int bit = (in[sx >> 3] >> (7 - (sx & 7))) & 1;
            black = white_is_one ? !bit : bit;
        } else {
            int v = in[sx];                     /* 8-bit */
            int ink = white_is_one ? 255 - v : v;
            black = ink * 64 > (bayer8[y & 7][x & 7] * 255 + 127);
        }
        if (black)
            out[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
}

/* Mirror a row horizontally (for upside-down printing). */
static void mirror_row(const uint8_t *in, uint8_t *out, int width)
{
    memset(out, 0, (size_t)(width + 7) / 8);
    for (int x = 0; x < width; x++)
        if (in[x >> 3] & (0x80 >> (x & 7))) {
            int m = width - 1 - x;
            out[m >> 3] |= (uint8_t)(0x80 >> (m & 7));
        }
}

/* ---------------------------------------------------------------- main */

int main(int argc, char *argv[])
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }

    int fd = 0;
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        perror("ERROR: Unable to open raster file");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);

    opt_ctx oc = {0};
    oc.num = cupsParseOptions(argv[5], 0, &oc.opts);
    const char *ppdname = getenv("PPD");
    if (ppdname && (oc.ppd = ppdOpenFile(ppdname)) != NULL) {
        ppdMarkDefaults(oc.ppd);
        cupsMarkOptions(oc.ppd, oc.num, oc.opts);
    }

    tm_options o;
    img_opts im;
    tm_default_options(&o);
    load_options(&oc, &o, &im);
    double calib = o.length_scale;

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t h;
    tm_encoder *enc = NULL;
    uint8_t *line = NULL, *row = NULL, *page_buf = NULL;
    int *xmap = NULL;
    size_t page_cap = 0;
    int page = 0, rc = 0;

    while (!g_cancel && cupsRasterReadHeader2(ras, &h)) {
        if (h.cupsBitsPerColor != 1 && h.cupsBitsPerColor != 8) {
            fprintf(stderr, "ERROR: Unsupported raster (%u bpc)\n", h.cupsBitsPerColor);
            rc = 1;
            break;
        }
        if (h.cupsBitsPerPixel != h.cupsBitsPerColor) {
            fprintf(stderr, "ERROR: Only single-channel raster supported\n");
            rc = 1;
            break;
        }

        double dpi = h.HWResolution[0] ? h.HWResolution[0] : 203;
        int head = im.head_dots;
        /* Imageable area of the page, in raster pixels. */
        double bb_l = h.ImagingBoundingBox[0], bb_r = h.ImagingBoundingBox[2];
        double page_w = h.PageSize[0];
        if (h.cupsImagingBBox[2] > 0) {             /* exact (float) values if present */
            bb_l = h.cupsImagingBBox[0];
            bb_r = h.cupsImagingBBox[2];
        }
        if (h.cupsPageSize[0] > 0)
            page_w = h.cupsPageSize[0];
        int xoff  = (int)lround(bb_l * dpi / 72.0);
        int src_w = (int)lround((bb_r - bb_l) * dpi / 72.0);
        if (src_w <= 0 || xoff + src_w > (int)h.cupsWidth)
            src_w = (int)h.cupsWidth - xoff;
        if (src_w <= 0) { xoff = 0; src_w = (int)h.cupsWidth; }

        /* Scaling: Reduced size print (70 %), Reduce 80 -> 58 mm, and
         * "Unsupported paper: automatically adjust and print" (a page wider than
         * the roll paper by more than 5 mm, e.g. A4/Letter, is shrunk to fit;
         * our reading of the APD option - Epson's exact rule is not traced). */
        double scale = im.page_scale;
        if (im.reduce_80_58 && head == 420)
            scale *= 420.0 / 576.0;
        int avail = head - im.left_dots;
        if (avail < 8) { im.left_dots = 0; avail = head; }
        double roll_pt = head == 420 ? 164.41 : 226.77;          /* 58 / 80 mm */
        if (im.unsupported_auto && page_w > roll_pt + 14.17 && src_w * scale > avail)
            scale = (double)avail / src_w;

        if (!enc) {
            o.width_dots = head;
            o.dpi_y = h.HWResolution[1] ? (int)h.HWResolution[1] : 203;
            o.length_scale = calib * scale;            /* vertical part of the scale */
            enc = tm_new(&o, write_stdout, NULL);
            xmap = malloc(sizeof(int) * (size_t)head);
            row = malloc((size_t)tm_row_bytes(enc));
            if (!enc || !xmap || !row) { fputs("ERROR: Out of memory\n", stderr); rc = 1; break; }
            fprintf(stderr, "DEBUG: head=%d xoff=%d src_w=%d left=%d scale=%.4f calib=%.4f "
                    "trim=%d blank=%d cut=%d/%d/%d/%d upside=%d\n",
                    head, xoff, src_w, im.left_dots, scale, calib, o.trim, o.blank_mode,
                    o.op[0].cut, o.op[1].cut, o.op[2].cut, o.op[3].cut, im.upside_down);
            tm_job_begin(enc);
        }
        uint8_t *nl = realloc(line, h.cupsBytesPerLine);
        if (!nl) { rc = 1; break; }
        line = nl;
        build_xmap(xmap, head, im.left_dots, xoff, xoff + src_w, scale);

        int rb = tm_row_bytes(enc);
        if (im.upside_down) {
            size_t need = (size_t)h.cupsHeight * (size_t)rb;
            if (need > page_cap) {
                uint8_t *p = realloc(page_buf, need);
                if (!p) { fputs("ERROR: Out of memory\n", stderr); rc = 1; break; }
                page_buf = p;
                page_cap = need;
            }
        }

        page++;
        fprintf(stderr, "PAGE: %d %d\n", page, h.NumCopies ? (int)h.NumCopies : 1);
        fprintf(stderr, "INFO: Printing page %d\n", page);
        tm_page_begin(enc);
        unsigned got = 0;
        for (unsigned y = 0; y < h.cupsHeight && !g_cancel; y++) {
            if (cupsRasterReadPixels(ras, line, h.cupsBytesPerLine) < 1)
                break;
            got++;
            if (im.upside_down) {
                convert_row(&h, line, page_buf + (size_t)y * rb, head, xmap, (int)y);
            } else {
                convert_row(&h, line, row, head, xmap, (int)y);
                if (tm_page_row(enc, row)) { rc = 1; break; }
            }
        }
        if (im.upside_down)                         /* rotate 180 degrees */
            for (unsigned y = got; y-- > 0 && !rc;) {
                mirror_row(page_buf + (size_t)y * rb, row, head);
                if (tm_page_row(enc, row)) rc = 1;
            }
        if (tm_page_end(enc)) rc = 1;
        if (rc) break;
    }

    if (enc) {
        if (tm_job_end(enc)) rc = 1;
        fprintf(stderr, "DEBUG: %ld rows output (%.1f mm)\n",
                tm_rows_emitted(enc), tm_rows_emitted(enc) * 25.4 / o.dpi_y);
    } else if (!rc) {
        fputs("ERROR: No pages found\n", stderr);
        rc = 1;
    }

    tm_free(enc);
    free(line);
    free(row);
    free(page_buf);
    free(xmap);
    cupsRasterClose(ras);
    if (oc.ppd) ppdClose(oc.ppd);
    cupsFreeOptions(oc.num, oc.opts);
    if (fd) close(fd);
    return rc;
}
