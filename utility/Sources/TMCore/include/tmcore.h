/*
 * tmcore.h - TM-T20III settings/status protocol (portable C).
 *
 * Implements the parts of Epson's TM-T20III Utility that talk to the printer
 * (see ../../../../RE_NOTES.md sections 3 and 6):
 *   GS ( E fn1/fn2  enter / leave user setting mode (leaving resets printer)
 *   GS ( E fn5/fn6  set / get customize values        (a, 16-bit value)
 *   GS ( E fn3/fn4  set / get memory switches         (Msw m, 8 bits)
 *   DLE EOT n       real-time status
 *   GS I n          printer information
 */
#ifndef TMCORE_H
#define TMCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ transport */

typedef struct tm_link {
    void *ctx;
    /* 0 on success */
    int  (*write)(void *ctx, const uint8_t *buf, size_t n);
    /* bytes read (>0), 0 on timeout, <0 on error */
    int  (*read)(void *ctx, uint8_t *buf, size_t cap, int timeout_ms);
    void (*close)(void *ctx);
    /* parser state */
    uint8_t rx[512];
    size_t  rx_len;
} tm_link;

enum {
    TM_OK = 0,
    TM_ERR_IO = -1,
    TM_ERR_TIMEOUT = -2,
    TM_ERR_PROTO = -3,
    TM_ERR_ARG = -4,
    TM_ERR_NODEV = -5
};

int  tm_link_fd(tm_link *l, int rfd, int wfd);              /* POSIX fds   */
int  tm_link_tcp(tm_link *l, const char *host, int port);   /* port 9100   */
int  tm_link_usb(tm_link *l);                               /* macOS only  */
void tm_link_close(tm_link *l);
const char *tm_strerror(int err);

/* ------------------------------------------------------------- commands */

int tm_send(tm_link *l, const uint8_t *buf, size_t n);

/* DLE EOT n (n = 1..4); returns the status byte in *out. */
int tm_status(tm_link *l, int n, uint8_t *out);

typedef struct {
    int online;
    int cover_open, feed_button, paper_end_stop, error;     /* n=2 */
    int cutter_error, unrecoverable, auto_recoverable;      /* n=3 */
    int near_end, paper_end;                                /* n=4 */
} tm_status_t;
int tm_status_all(tm_link *l, tm_status_t *s);

/* GS I n: n = 0x41 firmware, 0x42 maker, 0x43 model, 0x44 serial,
 * 0x45 font; n = 1..3 return one byte as decimal text. */
int tm_info(tm_link *l, int n, char *out, size_t cap);

int tm_setting_enter(tm_link *l);          /* fn1 "IN"                   */
int tm_setting_exit(tm_link *l);           /* fn2 "OUT" - printer resets */

int tm_customize_get(tm_link *l, int a, long *value);                   /* fn6 */
int tm_customize_set(tm_link *l, const int *a, const long *v, int k);  /* fn5 */
int tm_msw_get(tm_link *l, int m, uint8_t *bits);    /* fn4, bit7 = Msw m-8 */
int tm_msw_set(tm_link *l, int m, uint8_t mask, uint8_t value);         /* fn3 */

/* --------------------------------------------------------------- catalog */

/* TM_KIND_PAIR: two customize values in one setting, value = (b << 16) | a,
 * with a = id and b = customize value number `bit` (Windows 13110 "Paper
 * width - Number of columns" = a3 width | a11 column emulation). */
typedef enum { TM_KIND_CUSTOM = 0, TM_KIND_MSW = 1, TM_KIND_PAIR = 2 } tm_kind;

typedef struct { const char *label; long value; } tm_choice;

typedef struct {
    const char      *key;       /* "density"                                */
    const char      *title;     /* "Print density"                          */
    const char      *group;     /* Windows utility page: "Printing Control" */
    tm_kind          kind;
    int              id;        /* customize a, or Msw number               */
    int              bit;       /* Msw bit 1..8 (MSW only)                  */
    int              verified;  /* 1 = mapping confirmed, 0 = inferred      */
    long             min, max;  /* numeric range when choices == NULL       */
    const tm_choice *choices;   /* NULL-terminated                          */
} tm_setting;

extern const tm_setting tm_settings[];
extern const int        tm_settings_count;
const tm_setting *tm_setting_find(const char *key);
const tm_setting *tm_setting_at(int i);                /* Swift-friendly */
const char *tm_setting_label(const tm_setting *s, long value);   /* or NULL */
/* "Level 4" / "4" / "80mm" -> value; returns 0 on success */
int tm_setting_parse(const tm_setting *s, const char *text, long *value);

/* Read one catalog setting (works outside user setting mode). */
int tm_setting_read(tm_link *l, const tm_setting *s, long *value);

/* Apply key/value pairs: enter, write customize values in one fn5, write
 * each touched memory switch with a mask, leave.  The printer resets at the
 * end (USB will re-enumerate). */
typedef struct { const tm_setting *s; long value; } tm_assign;
int tm_apply(tm_link *l, const tm_assign *v, int n);

/* Presets: NULL-terminated "key=value" lists. */
typedef struct { const char *name; const char *title; const char *const *items; } tm_preset;
extern const tm_preset tm_presets[];
const tm_preset *tm_preset_find(const char *name);
const tm_preset *tm_preset_at(int i);                  /* NULL past the end */

/* ------------------------------------------------------------ test pages */

int tm_print_test(tm_link *l, const char *text);   /* text + feed + cut */
int tm_kick_drawer(tm_link *l, int pin);           /* pin 2 or 5        */
int tm_buzz(tm_link *l, int pattern, int count);   /* ESC ( A           */

#ifdef __cplusplus
}
#endif
#endif
