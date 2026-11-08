/*
 * tmcore.c - TM-T20III settings/status protocol + settings catalog.
 */
#include "tmcore.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* --------------------------------------------------------------- utils */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

const char *tm_strerror(int err)
{
    switch (err) {
    case TM_OK:          return "ok";
    case TM_ERR_IO:      return "I/O error";
    case TM_ERR_TIMEOUT: return "printer did not answer";
    case TM_ERR_PROTO:   return "unexpected reply";
    case TM_ERR_ARG:     return "invalid argument";
    case TM_ERR_NODEV:   return "printer not found";
    default:             return "error";
    }
}

int tm_send(tm_link *l, const uint8_t *buf, size_t n)
{
    if (!l || !l->write)
        return TM_ERR_ARG;
    return l->write(l->ctx, buf, n) == 0 ? TM_OK : TM_ERR_IO;
}

void tm_link_close(tm_link *l)
{
    if (l && l->close)
        l->close(l->ctx);
    if (l) {
        l->close = NULL;
        l->ctx = NULL;
    }
}

/* Read more bytes into the rx buffer.  Returns >0 bytes, 0 timeout, <0 err. */
static int fill(tm_link *l, int timeout_ms)
{
    if (l->rx_len == sizeof l->rx) {          /* buffer full of junk: drop half */
        memmove(l->rx, l->rx + sizeof l->rx / 2, sizeof l->rx / 2);
        l->rx_len = sizeof l->rx / 2;
    }
    int n = l->read(l->ctx, l->rx + l->rx_len, sizeof l->rx - l->rx_len, timeout_ms);
    if (n > 0)
        l->rx_len += (size_t)n;
    return n;
}

static void consume(tm_link *l, size_t n)
{
    if (n >= l->rx_len) {
        l->rx_len = 0;
        return;
    }
    memmove(l->rx, l->rx + n, l->rx_len - n);
    l->rx_len -= n;
}

/* Discard anything already waiting (ASB packets, power-on notices...). */
static void drain(tm_link *l)
{
    l->rx_len = 0;
    while (fill(l, 30) > 0)
        l->rx_len = 0;
    l->rx_len = 0;
}

/* Wait for "<prefix> ... 00"; copy the bytes between prefix and NUL. */
static int expect_block(tm_link *l, const uint8_t *prefix, size_t plen,
                        uint8_t *out, size_t cap, size_t *outlen, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        for (size_t i = 0; i + plen <= l->rx_len; i++) {
            if (memcmp(l->rx + i, prefix, plen) != 0)
                continue;
            for (size_t j = i + plen; j < l->rx_len; j++) {
                if (l->rx[j] != 0)
                    continue;
                size_t n = j - (i + plen);
                if (n > cap)
                    n = cap;
                if (out)
                    memcpy(out, l->rx + i + plen, n);
                if (outlen)
                    *outlen = n;
                consume(l, j + 1);
                return TM_OK;
            }
            break;                              /* prefix seen, need more */
        }
        long left = deadline - now_ms();
        if (left <= 0)
            return TM_ERR_TIMEOUT;
        int r = fill(l, (int)left);
        if (r < 0)
            return TM_ERR_IO;
    }
}

/* ------------------------------------------------------------- status */

int tm_status(tm_link *l, int n, uint8_t *out)
{
    if (n < 1 || n > 4)
        return TM_ERR_ARG;
    drain(l);
    uint8_t cmd[3] = {0x10, 0x04, (uint8_t)n};
    int rc = tm_send(l, cmd, 3);
    if (rc)
        return rc;
    long deadline = now_ms() + 1500;
    for (;;) {
        for (size_t i = 0; i < l->rx_len; i++) {
            /* DLE EOT reply: bit0=0, bit1=1, bit4=1, bit7=0 */
            if ((l->rx[i] & 0x93) == 0x12) {
                *out = l->rx[i];
                consume(l, i + 1);
                return TM_OK;
            }
        }
        long left = deadline - now_ms();
        if (left <= 0)
            return TM_ERR_TIMEOUT;
        if (fill(l, (int)left) < 0)
            return TM_ERR_IO;
    }
}

int tm_status_all(tm_link *l, tm_status_t *s)
{
    uint8_t b1, b2, b3, b4;
    int rc;
    memset(s, 0, sizeof *s);
    if ((rc = tm_status(l, 1, &b1)) || (rc = tm_status(l, 2, &b2)) ||
        (rc = tm_status(l, 3, &b3)) || (rc = tm_status(l, 4, &b4)))
        return rc;
    s->online           = !(b1 & 0x08);
    s->cover_open       = !!(b2 & 0x04);
    s->feed_button      = !!(b2 & 0x08);
    s->paper_end_stop   = !!(b2 & 0x20);
    s->error            = !!(b2 & 0x40);
    s->cutter_error     = !!(b3 & 0x08);
    s->unrecoverable    = !!(b3 & 0x20);
    s->auto_recoverable = !!(b3 & 0x40);
    s->near_end         = (b4 & 0x0C) != 0;
    s->paper_end        = (b4 & 0x60) != 0;
    return TM_OK;
}

int tm_info(tm_link *l, int n, char *out, size_t cap)
{
    if (!out || cap == 0)
        return TM_ERR_ARG;
    out[0] = 0;
    drain(l);
    uint8_t cmd[3] = {0x1D, 0x49, (uint8_t)n};
    int rc = tm_send(l, cmd, 3);
    if (rc)
        return rc;
    if (n >= 1 && n <= 3) {                       /* single byte */
        long deadline = now_ms() + 1500;
        while (l->rx_len == 0) {
            long left = deadline - now_ms();
            if (left <= 0)
                return TM_ERR_TIMEOUT;
            if (fill(l, (int)left) < 0)
                return TM_ERR_IO;
        }
        snprintf(out, cap, "%u", l->rx[0]);
        consume(l, 1);
        return TM_OK;
    }
    static const uint8_t hdr[1] = {0x5F};
    size_t len = 0;
    rc = expect_block(l, hdr, 1, (uint8_t *)out, cap - 1, &len, 1500);
    if (rc)
        return rc;
    out[len] = 0;
    return TM_OK;
}

/* ----------------------------------------------------- user setting mode */

int tm_setting_enter(tm_link *l)
{
    static const uint8_t cmd[] = {0x1D, 0x28, 0x45, 0x03, 0x00, 0x01, 0x49, 0x4E};
    static const uint8_t ack[] = {0x37, 0x20};
    drain(l);
    int rc = tm_send(l, cmd, sizeof cmd);
    if (rc)
        return rc;
    /* Notification "37 20 00"; tolerate its absence (some firmwares). */
    rc = expect_block(l, ack, 2, NULL, 0, NULL, 1500);
    return rc == TM_ERR_TIMEOUT ? TM_OK : rc;
}

int tm_setting_exit(tm_link *l)
{
    static const uint8_t cmd[] = {0x1D, 0x28, 0x45, 0x04, 0x00, 0x02, 0x4F, 0x55, 0x54};
    return tm_send(l, cmd, sizeof cmd);
}

int tm_customize_get(tm_link *l, int a, long *value)
{
    if (a < 1 || a > 255 || !value)
        return TM_ERR_ARG;
    drain(l);
    uint8_t cmd[7] = {0x1D, 0x28, 0x45, 0x02, 0x00, 0x06, (uint8_t)a};
    int rc = tm_send(l, cmd, sizeof cmd);
    if (rc)
        return rc;
    static const uint8_t hdr[2] = {0x37, 0x27};
    uint8_t buf[32];
    size_t n = 0;
    if ((rc = expect_block(l, hdr, 2, buf, sizeof buf - 1, &n, 1500)))
        return rc;
    buf[n] = 0;
    char *sep = memchr(buf, 0x1F, n);
    if (!sep)
        return TM_ERR_PROTO;
    *sep = 0;
    if (atoi((char *)buf) != a)
        return TM_ERR_PROTO;
    *value = strtol(sep + 1, NULL, 10);
    return TM_OK;
}

int tm_customize_set(tm_link *l, const int *a, const long *v, int k)
{
    while (k > 0) {
        int chunk = k > 64 ? 64 : k;
        uint8_t cmd[6 + 3 * 64];
        int len = 3 * chunk + 1;
        cmd[0] = 0x1D; cmd[1] = 0x28; cmd[2] = 0x45;
        cmd[3] = (uint8_t)len; cmd[4] = (uint8_t)(len >> 8); cmd[5] = 0x05;
        for (int i = 0; i < chunk; i++) {
            if (a[i] < 1 || a[i] > 255 || v[i] < 0 || v[i] > 65535)
                return TM_ERR_ARG;
            cmd[6 + 3 * i]     = (uint8_t)a[i];
            cmd[6 + 3 * i + 1] = (uint8_t)v[i];
            cmd[6 + 3 * i + 2] = (uint8_t)(v[i] >> 8);
        }
        int rc = tm_send(l, cmd, (size_t)(6 + 3 * chunk));
        if (rc)
            return rc;
        a += chunk;
        v += chunk;
        k -= chunk;
    }
    return TM_OK;
}

int tm_msw_get(tm_link *l, int m, uint8_t *bits)
{
    if (m < 1 || m > 255 || !bits)
        return TM_ERR_ARG;
    drain(l);
    uint8_t cmd[7] = {0x1D, 0x28, 0x45, 0x02, 0x00, 0x04, (uint8_t)m};
    int rc = tm_send(l, cmd, sizeof cmd);
    if (rc)
        return rc;
    static const uint8_t hdr[2] = {0x37, 0x21};
    uint8_t buf[16];
    size_t n = 0;
    if ((rc = expect_block(l, hdr, 2, buf, sizeof buf, &n, 1500)))
        return rc;
    if (n != 8)
        return TM_ERR_PROTO;
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {                 /* buf[0] = bit 8 */
        if (buf[i] != '0' && buf[i] != '1')
            return TM_ERR_PROTO;
        if (buf[i] == '1')
            v |= (uint8_t)(0x80 >> i);
    }
    *bits = v;
    return TM_OK;
}

int tm_msw_set(tm_link *l, int m, uint8_t mask, uint8_t value)
{
    if (m < 1 || m > 255)
        return TM_ERR_ARG;
    uint8_t cmd[15] = {0x1D, 0x28, 0x45, 0x0A, 0x00, 0x03, (uint8_t)m};
    for (int i = 0; i < 8; i++) {                 /* b8 .. b1 */
        uint8_t bit = (uint8_t)(0x80 >> i);
        cmd[7 + i] = (mask & bit) ? ((value & bit) ? '1' : '0') : '2';
    }
    return tm_send(l, cmd, sizeof cmd);
}

/* --------------------------------------------------------------- catalog */

/* Labels and values exactly as TM-T20III Utility (Lang/0409.DAT
 * [TMUCTMPRNSTG_DLL_CMB], [TMUPAPERSTG_DLL_CMB], [TMUFNTSET_DLL_CMB]). */
static const tm_choice ch_widthcol[] = {{"80mm - 48 columns", 0x00000006}, {"80mm - 42 columns", 0x00010006},
                                        {"58mm - 35 columns", 0x00000002}, {"58mm - 42 columns", 0x00010002},
                                        {0, 0}};
static const tm_choice ch_dens[]   = {{"Level 1 (Light)", 1}, {"Level 2", 2}, {"Level 3", 3},
                                      {"Level 4", 4}, {"Level 5", 5}, {"Level 6", 6},
                                      {"Level 7 (Dark)", 7}, {0, 0}};
static const tm_choice ch_speed[]  = {{"Level 1 (Slow)", 1}, {"Level 2", 2}, {"Level 3", 3},
                                      {"Level 4", 4}, {"Level 5", 5}, {"Level 6", 6},
                                      {"Level 7", 7}, {"Level 8", 8}, {"Level 9", 9},
                                      {"Level 10", 10}, {"Level 11", 11}, {"Level 12", 12},
                                      {"Level 13 (Fast)", 13}, {0, 0}};
static const tm_choice ch_onoff[]  = {{"Disabled", 0}, {"Enabled", 1}, {0, 0}};
static const tm_choice ch_resize[] = {{"None", 0}, {"Auto", 1}, {"Manual", 2}, {0, 0}};
static const tm_choice ch_algo[]   = {{"Simple thinning", 0}, {"Nearest neighbor", 1},
                                      {"Bilinear", 2}, {"Bicubic", 3}, {0, 0}};
static const tm_choice ch_psu[]    = {{"Level 1 (Low)", 0}, {"Level 2", 1},
                                      {"Level 3 (High)", 2}, {0, 0}};
static const tm_choice ch_cutter[] = {{"Command Only", 0}, {"Command and Cover Close", 1}, {0, 0}};
static const tm_choice ch_endis[]  = {{"Disable", 0}, {"Enable", 1}, {0, 0}};
static const tm_choice ch_reduce[] = {{"Not reduced", 0}, {"25%", 1}, {"50%", 2}, {"75%", 3}, {0, 0}};
static const tm_choice ch_charh[]  = {{"Not reduced", 0}, {"Reduce Letter-spacing by 25%", 1},
                                      {"Reduce Letter-spacing by 50%", 2},
                                      {"Reduce Letter-spacing by 75%", 3}, {0, 0}};
/* Windows also offers "128 KB" (value 2); a single memory-switch bit cannot
 * hold it and its encoding is not traced, so it is left out. */
static const tm_choice ch_rxbuf[]  = {{"4 KB", 0}, {"45 bytes", 1}, {0, 0}};
static const tm_choice ch_busy[]   = {{"Offline or Receive buffer full", 0},
                                      {"Receive buffer full", 1}, {0, 0}};
static const tm_choice ch_rxerr[]  = {{"Prints '?'", 0}, {"Ignored", 1}, {0, 0}};
static const tm_choice ch_autolf[] = {{"Always disabled", 0}, {"Always enabled", 1}, {0, 0}};
static const tm_choice ch_bsyrel[] = {
    {"Cancels the BUSY state when the remaining capacity of the receive buffer reaches 256 bytes.", 0},
    {"Cancels the BUSY state when the remaining capacity of the receive buffer reaches 138 bytes.", 1},
    {0, 0}};
static const tm_choice ch_pesens[] = {{"Disabled", 0},
    {"Roll paper end sensor enabled, Roll paper near-end sensor enabled", 1}, {0, 0}};
static const tm_choice ch_codepg[] = {
    {"PC437 (USA:Standard Europe)", 0}, {"Katakana", 1}, {"PC850 (Multilingual)", 2},
    {"PC860 (Portuguese)", 3}, {"PC863 (Canadian-French)", 4}, {"PC865 (Nordic)", 5},
    {"PC851 (Greek)", 11}, {"PC853 (Turkish)", 12}, {"PC857 (Turkish)", 13}, {"PC737 (Greek)", 14},
    {"ISO8859-7(Greek)", 15}, {"WPC1252", 16}, {"PC866 (Cyrillic #2)", 17}, {"PC852 (Latin 2)", 18},
    {"PC858 (Euro)", 19}, {"KU42", 20}, {"TIS11 (Thai)", 21}, {"TIS18 (Thai)", 26},
    {"TCVN-3 (Vietnamese)", 30}, {"TCVN-3 (Vietnamese Capital)", 31}, {"PC720", 32}, {"WPC775", 33},
    {"PC855 (Cyrillic)", 34}, {"PC861 (Icelandic)", 35}, {"PC862 (Hebrew)", 36}, {"PC864 (Arabic)", 37},
    {"PC869 (Greek)", 38}, {"ISO8859-2 (Latin2)", 39}, {"ISO8859-15 (Latin9)", 40},
    {"PC1098 (Farsi)", 41}, {"PC1118 (Lithuanian)", 42}, {"PC1119 (Lithuanian)", 43},
    {"PC1125 (Ukrainian)", 44}, {"WPC1250", 45}, {"WPC1251", 46}, {"WPC1253", 47}, {"WPC1254", 48},
    {"WPC1255", 49}, {"WPC1256", 50}, {"WPC1257", 51}, {"WPC1258", 52}, {"KZ-1048 (Kazakhstan)", 53},
    {"User-defined page", 255}, {0, 0}};
static const tm_choice ch_intl[]   = {
    {"USA", 0}, {"France", 1}, {"Germany", 2}, {"UK", 3}, {"Denmark I", 4}, {"Sweden", 5},
    {"Italy", 6}, {"Spain I", 7}, {"Japan", 8}, {"Norway", 9}, {"Denmark II", 10}, {"Spain II", 11},
    {"Latin America", 12}, {"Korea", 13}, {"Slovenia / Croatia", 14}, {"China", 15},
    {"Vietnam", 16}, {"Arabia", 17}, {0, 0}};

#define CUST(k, t, g, a, ver, ch)        {k, t, g, TM_KIND_CUSTOM, a, 0, ver, 0, 0, ch}
#define CUSTR(k, t, g, a, ver, lo, hi)   {k, t, g, TM_KIND_CUSTOM, a, 0, ver, lo, hi, NULL}
#define MSW(k, t, g, m, b, ver, ch)      {k, t, g, TM_KIND_MSW, m, b, ver, 0, 0, ch}

/* Order and titles = Windows "Printing Control" list (Function | Setting). */
const tm_setting tm_settings[] = {
    {"paper_width_columns", "Paper width - Number of columns", "Printing Control",
     TM_KIND_PAIR, 3, 11, 1, 0, 0, ch_widthcol},
    CUST ("density",           "Print density",                                "Printing Control", 5, 1, ch_dens),
    CUST ("speed",             "Print speed",                                  "Printing Control", 6, 1, ch_speed),
    CUST ("power_led",         "Power LED indicator when interface starting",  "Printing Control", 22, 1, ch_onoff),
    CUST ("resize_method",     "Resizing method for graphic data scaling",     "Printing Control", 70, 1, ch_resize),
    CUST ("resize_algorithm",  "    Algorithm",                                "Printing Control", 71, 1, ch_algo),
    CUSTR("resize_ratio",      "    Scaling factor for manual adjustment (%)", "Printing Control", 73, 1, 1, 200),
    CUST ("power_supply",      "Power Supply Unit Capacity",                   "Printing Control", 98, 1, ch_psu),
    CUST ("autocut_cover",     "Method for canceling auto cutter error",       "Printing Control", 100, 1, ch_cutter),
    MSW  ("rx_buffer",         "Receive buffer capacity",                      "Printing Control", 1, 2, 1, ch_rxbuf),
    MSW  ("busy_condition",    "Handshaking (Condition for BUSY)",             "Printing Control", 1, 3, 1, ch_busy),
    MSW  ("rx_error",          "Data reception error",                         "Printing Control", 1, 4, 1, ch_rxerr),
    MSW  ("auto_lf",           "Auto line feed",                               "Printing Control", 1, 5, 1, ch_autolf),
    MSW  ("usb_power_save",    "Power saving function for USB",                "Printing Control", 5, 1, 0, ch_onoff),
    MSW  ("busy_release",      "Setting the conditions that cancel the receive buffer BUSY state",
                                                                               "Printing Control", 5, 2, 0, ch_bsyrel),
    MSW  ("paper_end_signal",  "Selects paper sensors to output paper-end signals", "Printing Control", 5, 3, 0, ch_pesens),
    MSW  ("error_signal",      "Error signal output",                          "Printing Control", 5, 4, 0, ch_onoff),
    MSW  ("prefeed",           "Pre-feed before next print",                   "Printing Control", 5, 5, 0, ch_onoff),
    MSW  ("near_end_sensor",   "Roll paper near-end sensor",                   "Printing Control", 6, 1, 0, ch_onoff),
    MSW  ("error_signal_8",    "Error signal output (Msw8-6)",                 "Printing Control", 8, 6, 0, ch_onoff),
    /* Paper Reduction page (TMUPAPERSTG) */
    CUST ("reduce_top",        "Extra Upper Space Reduction",                  "Paper Reduction", 101, 1, ch_endis),
    CUST ("reduce_bottom",     "Extra Lower Space Reduction",                  "Paper Reduction", 102, 1, ch_endis),
    CUST ("reduce_line_space", "Line Space Reduction Rate",                    "Paper Reduction", 103, 1, ch_reduce),
    CUST ("reduce_line_feed",  "Line Feed Reduction Rate",                     "Paper Reduction", 104, 1, ch_reduce),
    CUST ("reduce_barcode",    "Barcode Height Reduction Rate",                "Paper Reduction", 105, 1, ch_reduce),
    CUST ("reduce_char_height","Reduction ratio of character height",          "Paper Reduction", 106, 1, ch_charh),
    /* Font page (TMUFont): default code page / international character set
     * (customize 8 / 9 per Epson's generic GS ( E table; not confirmed). */
    CUST ("code_page",         "Code-page",                                    "Font", 8, 0, ch_codepg),
    CUST ("intl_charset",      "International character set",                  "Font", 9, 0, ch_intl),
};
const int tm_settings_count = (int)(sizeof tm_settings / sizeof tm_settings[0]);

const tm_setting *tm_setting_find(const char *key)
{
    for (int i = 0; i < tm_settings_count; i++)
        if (!strcasecmp(tm_settings[i].key, key))
            return &tm_settings[i];
    return NULL;
}

const tm_setting *tm_setting_at(int i)
{
    return (i >= 0 && i < tm_settings_count) ? &tm_settings[i] : NULL;
}

const char *tm_setting_label(const tm_setting *s, long value)
{
    if (!s || !s->choices)
        return NULL;
    for (const tm_choice *c = s->choices; c->label; c++)
        if (c->value == value)
            return c->label;
    return NULL;
}

int tm_setting_parse(const tm_setting *s, const char *text, long *value)
{
    if (!s || !text || !*text)
        return TM_ERR_ARG;
    if (s->choices) {
        for (const tm_choice *c = s->choices; c->label; c++)       /* exact label */
            if (!strcasecmp(c->label, text)) {
                *value = c->value;
                return TM_OK;
            }
        size_t tl = strlen(text);
        const tm_choice *hit = NULL;
        for (const tm_choice *c = s->choices; c->label; c++)       /* unique prefix */
            if (!strncasecmp(c->label, text, tl) &&
                !isdigit((unsigned char)c->label[tl])) {   /* "Level 1" != "Level 10" */
                if (hit)
                    { hit = NULL; break; }
                hit = c;
            }
        if (hit) {
            *value = hit->value;
            return TM_OK;
        }
    }
    char *end = NULL;
    long v = strtol(text, &end, 0);
    if (end == text)
        return TM_ERR_ARG;
    if (s->choices) {
        if (!tm_setting_label(s, v)) {
            /* "4" for "Level 4"-style labels: a number matching exactly one
             * label prefix */
            const tm_choice *hit = NULL;
            int hits = 0;
            for (const tm_choice *c = s->choices; c->label; c++)
                if (atol(c->label) == v && isdigit((unsigned char)c->label[0])) {
                    hit = c;
                    hits++;
                }
            if (hits != 1)
                return TM_ERR_ARG;
            *value = hit->value;
            return TM_OK;
        }
    } else if (v < s->min || v > s->max) {
        return TM_ERR_ARG;
    }
    *value = v;
    return TM_OK;
}

int tm_setting_read(tm_link *l, const tm_setting *s, long *value)
{
    if (s->kind == TM_KIND_CUSTOM)
        return tm_customize_get(l, s->id, value);
    if (s->kind == TM_KIND_PAIR) {
        long a = 0, b = 0;
        int rc = tm_customize_get(l, s->id, &a);
        if (rc == TM_OK)
            rc = tm_customize_get(l, s->bit, &b);
        if (rc == TM_OK)
            *value = (b << 16) | (a & 0xFFFF);
        return rc;
    }
    uint8_t bits = 0;
    int rc = tm_msw_get(l, s->id, &bits);
    if (rc == TM_OK)
        *value = (bits >> (s->bit - 1)) & 1;
    return rc;
}

int tm_apply(tm_link *l, const tm_assign *v, int n)
{
    int  a[64];
    long val[64];
    int  k = 0;
    uint8_t mask[9] = {0}, bits[9] = {0};

    for (int i = 0; i < n; i++) {
        const tm_setting *s = v[i].s;
        if (!s)
            return TM_ERR_ARG;
        if (s->kind == TM_KIND_CUSTOM || s->kind == TM_KIND_PAIR) {
            int pair = s->kind == TM_KIND_PAIR;
            if (k + pair >= 64 || s->id == 1 || s->id == 2 ||
                (pair && (s->bit == 1 || s->bit == 2)))   /* never touch NV sizes */
                return TM_ERR_ARG;
            a[k] = s->id;
            val[k++] = pair ? (v[i].value & 0xFFFF) : v[i].value;
            if (pair) {
                a[k] = s->bit;
                val[k++] = (v[i].value >> 16) & 0xFFFF;
            }
        } else {
            if (s->id < 1 || s->id > 8 || s->bit < 1 || s->bit > 8)
                return TM_ERR_ARG;
            uint8_t b = (uint8_t)(1 << (s->bit - 1));
            mask[s->id] |= b;
            if (v[i].value)
                bits[s->id] |= b;
        }
    }

    int rc = tm_setting_enter(l);
    if (rc)
        return rc;
    if (k)
        rc = tm_customize_set(l, a, val, k);
    for (int m = 1; m <= 8 && rc == TM_OK; m++)
        if (mask[m])
            rc = tm_msw_set(l, m, mask[m], bits[m]);
    int rc2 = tm_setting_exit(l);                    /* always leave the mode */
    return rc ? rc : rc2;
}

/* --------------------------------------------------------------- presets */

static const char *const preset_truss[] = {
    /* Paperwork.show "Epson TM-T20III Setup" (settings verified in their
     * screenshots).  Memory switches 5/6/8 omitted until confirmed. */
    "paper_width_columns=80mm - 48 columns", "density=4", "speed=10",
    "resize_method=None", "resize_algorithm=Nearest neighbor", "resize_ratio=100",
    "power_supply=2", "autocut_cover=1",
    "reduce_top=0", "reduce_bottom=0", "reduce_line_space=0", "reduce_line_feed=0",
    "reduce_barcode=0", "reduce_char_height=0",
    "rx_buffer=0", "busy_condition=0", "rx_error=0", "auto_lf=0",
    NULL
};

static const char *const preset_noreduce[] = {
    "reduce_top=0", "reduce_bottom=0", "reduce_line_space=0", "reduce_line_feed=0",
    "reduce_barcode=0", "reduce_char_height=0", NULL
};

const tm_preset tm_presets[] = {
    {"truss",    "Truss tapes (Paperwork.show)", preset_truss},
    {"noreduce", "Disable all paper reduction",  preset_noreduce},
    {NULL, NULL, NULL}
};

const tm_preset *tm_preset_find(const char *name)
{
    for (const tm_preset *p = tm_presets; p->name; p++)
        if (!strcasecmp(p->name, name))
            return p;
    return NULL;
}

const tm_preset *tm_preset_at(int i)
{
    for (int k = 0; tm_presets[k].name; k++)
        if (k == i)
            return &tm_presets[k];
    return NULL;
}

/* ------------------------------------------------------------ test pages */

int tm_print_test(tm_link *l, const char *text)
{
    static const uint8_t init[] = {0x1B, 0x40, 0x1B, 0x61, 0x01};   /* ESC @, center */
    static const uint8_t tail[] = {0x0A, 0x0A, 0x1D, 0x56, 0x41, 0x00};
    int rc = tm_send(l, init, sizeof init);
    if (!rc && text)
        rc = tm_send(l, (const uint8_t *)text, strlen(text));
    if (!rc)
        rc = tm_send(l, tail, sizeof tail);
    return rc;
}

int tm_kick_drawer(tm_link *l, int pin)
{
    if (pin != 2 && pin != 5)
        return TM_ERR_ARG;
    uint8_t cmd[5] = {0x1B, 0x70, (uint8_t)(pin == 2 ? 0 : 1), 0x32, 0xC8};
    return tm_send(l, cmd, sizeof cmd);
}

int tm_buzz(tm_link *l, int pattern, int count)
{
    if (pattern < 1 || pattern > 7 || count < 1 || count > 255)
        return TM_ERR_ARG;
    uint8_t cmd[8] = {0x1B, 0x28, 0x41, 0x03, 0x00, 0x61, (uint8_t)pattern, (uint8_t)count};
    return tm_send(l, cmd, sizeof cmd);
}
