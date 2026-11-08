/*
 * tmt20.h - ESC/POS raster encoder for the Epson TM-T20III.
 *
 * Portable C99 + zlib, no CUPS dependency.  Byte formats come from reverse
 * engineering Epson's APD6 driver (see ../../RE_NOTES.md):
 *   - raster bands: GS 8 L fn112 (store) + GS ( L fn50 (print)
 *   - zlib-compressed bands (a=0x31) with raw fallback (a=0x30)
 *   - raw bands sanitised so pixel data cannot trigger real-time commands
 *   - cut / drawer / buzzer commands from the model's PrinterSpec table
 */
#ifndef TMT20_H
#define TMT20_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cut modes = keys of the PrinterSpec command_cut_<n> table. */
enum {
    TM_CUT_NONE          = 0,  /* ESC J 0                      */
    TM_CUT_NOFEED_CUT    = 1,  /* ESC J 0, GS V 0              */
    TM_CUT_FEED_NOCUT    = 16, /* ESC J 255, ESC J 193         */
    TM_CUT_FEED_CUT      = 17  /* ESC J 0, GS V A 0            */
};

/* Peripheral ("dk") modes = keys of the PrinterSpec command_dk_<n> table. */
enum {
    TM_DK_NONE            = 0,
    TM_DK_DRAWER1         = 1,    /* ESC p '0' 50 200   (pin 2)              */
    TM_DK_DRAWER2         = 2,    /* ESC p '1' 50 200   (pin 5)              */
    TM_DK_DRAWER12        = 3,
    TM_DK_BUZZER          = 256,  /* built-in buzzer (pulse on pin 5)        */
    TM_DK_BUZZER_DRAWER1  = 257,
    TM_DK_BUZZER_DRAWER2  = 258,
    TM_DK_EXT_BUZZER_A    = 512,  /* external buzzer pattern A..E = 512..516 */
    TM_DK_EXT_BUZZER_E    = 516
};

/* Operation timings, in the order the Windows driver keeps them. */
enum { TM_AT_DOC_BEGIN = 0, TM_AT_PAGE_BEGIN = 1, TM_AT_PAGE_END = 2, TM_AT_DOC_END = 3, TM_AT_COUNT = 4 };

#define TM_CMD_MAX 256

/* One "DoOperation" (Feed and Cut + Peripherals + Send Command) at a timing.
 * Emission order matches EA6RDPTM DoOperation: user command, drawer/buzzer,
 * extra feed, cut. */
typedef struct {
    int     cut;               /* TM_CUT_*                                     */
    int     feed_um;           /* extra feed before the cut (micrometres)      */
    int     dk;                /* TM_DK_*                                      */
    int     dk_count;          /* external buzzer repeat count 1..255          */
    size_t  cmd_len;           /* Send Command bytes                           */
    uint8_t cmd[TM_CMD_MAX];
} tm_op;

typedef enum {
    TM_TRIM_NONE   = 0,        /* feed every blank row (exact length)        */
    TM_TRIM_TOP    = 1,        /* drop blank rows before first ink on a page */
    TM_TRIM_BOTTOM = 2,        /* drop blank rows after last ink on a page   */
    TM_TRIM_BOTH   = 3         /*   (Epson's driver defaults to BOTTOM)      */
} tm_trim;

typedef enum {
    TM_BLANK_FEED   = 0,       /* blank rows -> ESC J (like Epson's driver)  */
    TM_BLANK_RASTER = 1        /* blank rows -> empty raster (exact length)  */
} tm_blank_mode;

typedef struct {
    int    width_dots;         /* printable width: 576 (80 mm) or 420 (58 mm) */
    int    dpi_y;              /* 203                                          */
    int    y_units_per_row;    /* ESC J units per raster row (1/406 in -> 2)   */
    int    band_rows;          /* rows per GS 8 L band (Epson: 256)            */
    int    compress;           /* 1 = zlib bands when safe                     */
    double length_scale;       /* vertical scale: calibration x reduction       */
    tm_trim       trim;
    tm_blank_mode blank_mode;

    int    top_margin_um;      /* Adjust Print Position: feed before each page */
    int    print_control;      /* 0 = printer default, 1..3 (GS ( K fn48)      */
    tm_op  op[TM_AT_COUNT];    /* default: Document[Feed, Cut]                 */
} tm_options;

/* Must return the number of bytes written (== len on success). */
typedef size_t (*tm_write_fn)(void *ctx, const uint8_t *buf, size_t len);

typedef struct tm_encoder tm_encoder;

void        tm_default_options(tm_options *o);
tm_encoder *tm_new(const tm_options *o, tm_write_fn write, void *ctx);
void        tm_free(tm_encoder *e);

int tm_job_begin(tm_encoder *e);
int tm_page_begin(tm_encoder *e);
/* One raster row: width_dots bits, MSB = leftmost dot, 1 = black. */
int tm_page_row(tm_encoder *e, const uint8_t *row);
int tm_page_end(tm_encoder *e);
int tm_job_end(tm_encoder *e);

int  tm_row_bytes(const tm_encoder *e);
long tm_rows_emitted(const tm_encoder *e);   /* raster+fed rows so far (stats) */

/* Feed and Cut presets (Windows "Preset option" list).  Applying one sets
 * the cut mode of every timing and zeroes the extra feeds. */
typedef enum {
    TM_PRESET_DOC_FEED_CUT = 0, TM_PRESET_DOC_FEED_NOCUT, TM_PRESET_DOC_NOFEED_CUT,
    TM_PRESET_DOC_NOFEED_NOCUT, TM_PRESET_PAGE_FEED_CUT, TM_PRESET_PAGE_FEED_NOCUT,
    TM_PRESET_PAGE_NOFEED_CUT, TM_PRESET_COUNT
} tm_feedcut_preset;
void tm_apply_feedcut_preset(tm_options *o, tm_feedcut_preset p);

/* Send Command text -> bytes: hex pairs ("1B 40", "1b40", "0x1B", "<1B>")
 * and "quoted text".  Returns the byte count, or -1 if invalid / too long. */
int tm_parse_cmd(const char *text, uint8_t *out, size_t cap);

/* Exposed for tests. */
int  tm_has_realtime_dc4(const uint8_t *p, size_t n);   /* FUN_180057bf0 */
void tm_sanitize_raw(uint8_t *p, size_t n);             /* FUN_180057ba0 */

#ifdef __cplusplus
}
#endif
#endif
