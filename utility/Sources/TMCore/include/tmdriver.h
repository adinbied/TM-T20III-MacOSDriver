/*
 * tmdriver.h - Printing Preferences model for the TM-T20III CUPS queue.
 *
 * The Windows driver keeps its settings in the DEVMODE (Printing
 * Preferences tabs) and in printer data written by the APD Utility (Layout,
 * User Defined Paper, Send Command).  On macOS all of it lives in the
 * queue's PPD:
 *   - print-dialog options  -> "*Default<Option>: <choice>"     (PPD UI)
 *   - detail settings       -> "*TmSetting <key>: "<value>""   (read by the filter)
 *   - user defined papers   -> "*TmUserPaper <kw>/<name>: "..."" + generated
 *                              PageSize / PageRegion / ImageableArea /
 *                              PaperDimension entries
 * This module parses a queue PPD into that model, validates edits, and
 * generates a new PPD from the installed template.  Applying it is
 * "lpadmin -p <queue> -P <generated.ppd>" (admin rights).
 */
#ifndef TMDRIVER_H
#define TMDRIVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TM_DRV_MAX_KV     96
#define TM_DRV_MAX_PAPERS 32
#define TM_DRV_KEY_LEN    40
#define TM_DRV_VAL_LEN    600

typedef enum { TM_DRV_UI = 0, TM_DRV_SETTING = 1 } tm_drv_kind;

/* One known setting. choices: "a|b|c" (PPD choice keywords) or NULL for a
 * number in [min, max], or NULL with min > max for free text (commands). */
typedef struct {
    const char *key;
    tm_drv_kind kind;
    const char *def;
    const char *choices;
    long        min, max;
} tm_drv_desc;

const tm_drv_desc *tm_drv_desc_at(int i);           /* NULL past the end */
const tm_drv_desc *tm_drv_desc_find(const char *key);

typedef struct {
    char   keyword[24];        /* "UDP1"                                    */
    char   name[32];           /* "TRUSS TAPE" (Windows: max 31 chars)      */
    long   width_um, height_um;
    long   left_um, top_um, right_um, bottom_um;
    int    unit_inch;          /* display unit: 0 mm, 1 inch                */
} tm_drv_paper;

typedef struct {
    int          n;
    char         key[TM_DRV_MAX_KV][TM_DRV_KEY_LEN];
    char         val[TM_DRV_MAX_KV][TM_DRV_VAL_LEN];
    int          npapers;
    tm_drv_paper paper[TM_DRV_MAX_PAPERS];
} tm_drv;

/* Windows limits (TM-T20III.ini [UTL_USERDEFINEDPAPER], 0.1 mm units). */
#define TM_UDP_W_MIN_UM   52600
#define TM_UDP_W_MAX_UM  210000
#define TM_UDP_H_MIN_UM   25400
#define TM_UDP_H_MAX_UM 3276000

enum { TM_DRV_OK = 0, TM_DRV_EKEY = -1, TM_DRV_EVALUE = -2, TM_DRV_EFULL = -3,
       TM_DRV_ERANGE = -4, TM_DRV_EMARGIN = -5, TM_DRV_ENAME = -6, TM_DRV_ENOTFOUND = -7 };
const char *tm_drv_strerror(int err);

void        tm_drv_defaults(tm_drv *d);
const char *tm_drv_get(const tm_drv *d, const char *key);    /* value or default */
int         tm_drv_set(tm_drv *d, const char *key, const char *value);
int         tm_drv_is_default(const tm_drv *d, const char *key);

/* Add or replace (same name, case-insensitive) a user defined paper. */
int tm_drv_paper_add(tm_drv *d, const tm_drv_paper *p);
int tm_drv_paper_del(tm_drv *d, const char *name);
const tm_drv_paper *tm_drv_paper_at(const tm_drv *d, int i);

/* Read settings from a queue PPD (defaults first, then what the PPD holds). */
void tm_drv_parse_ppd(tm_drv *d, const char *ppd_text);

/* Generate a queue PPD from the installed template.  Returns malloc'd text,
 * or NULL if the template has no @@ markers. */
char *tm_drv_generate_ppd(const char *template_text, const tm_drv *d);

/* Normalise Send Command text ("1B 40", "\"text\"", ...) to spaced hex
 * ("1B 40 74 65 78 74"), as stored in the PPD.  0 on success. */
int tm_drv_cmd_normalize(const char *text, char *out, size_t cap);

/* Swift-friendly helpers (no C arrays/tuples needed). */
tm_drv     *tm_drv_new(void);                       /* zeroed, defaults */
void        tm_drv_free(tm_drv *d);
tm_drv     *tm_drv_copy(const tm_drv *d);
int         tm_drv_paper_count(const tm_drv *d);
const char *tm_drv_paper_name(const tm_drv *d, int i);
const char *tm_drv_paper_keyword(const tm_drv *d, int i);
int         tm_drv_paper_add_simple(tm_drv *d, const char *name, long width_um, long height_um,
                                    long left_um, long top_um, long right_um, long bottom_um,
                                    int unit_inch);

#ifdef __cplusplus
}
#endif
#endif
