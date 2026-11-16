/*
 * tmsim.c - minimal TM-T20III protocol simulator for tests.
 * Speaks: DLE EOT n, GS I n, GS ( E fn1..fn6.  Injects ASB-style noise.
 */
#include "tmsim.h"

#include <pthread.h>
#include <string.h>
#include <unistd.h>

static void reply(tm_sim *s, const void *b, size_t n)
{
    if (s->noise) {                          /* unsolicited ASB-like packet */
        static const uint8_t asb[4] = {0x14, 0x00, 0x00, 0x0F};
        (void)!write(s->fd, asb, sizeof asb);
    }
    (void)!write(s->fd, b, n);
}

static int rd(tm_sim *s, uint8_t *b, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(s->fd, b + got, n - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static void gs_e(tm_sim *s)
{
    uint8_t h[3];
    if (rd(s, h, 3))
        return;
    size_t len = (size_t)(h[0] | h[1] << 8);
    uint8_t fn = h[2], body[1024];
    if (len < 1 || len - 1 > sizeof body || rd(s, body, len - 1))
        return;
    size_t bl = len - 1;
    char out[64];
    switch (fn) {
    case 1:
        s->in_setting = 1;
        reply(s, "\x37\x20\x00", 3);
        break;
    case 2:
        s->in_setting = 0;
        s->resets++;
        break;
    case 3:
        if (!s->in_setting) { s->ignored_writes++; break; }
        for (size_t i = 0; i + 9 <= bl; i += 9) {
            int m = body[i];
            for (int k = 0; k < 8; k++) {
                uint8_t bit = (uint8_t)(0x80 >> k);
                if (body[i + 1 + k] == '1') s->msw[m] |= bit;
                else if (body[i + 1 + k] == '0') s->msw[m] &= (uint8_t)~bit;
            }
        }
        break;
    case 4: {
        int m = body[0];
        out[0] = 0x37; out[1] = 0x21;
        for (int k = 0; k < 8; k++)
            out[2 + k] = (s->msw[m] & (0x80 >> k)) ? '1' : '0';
        out[10] = 0;
        reply(s, out, 11);
        break;
    }
    case 5:
        if (!s->in_setting) { s->ignored_writes++; break; }
        for (size_t i = 0; i + 3 <= bl; i += 3)
            s->custom[body[i]] = body[i + 1] | body[i + 2] << 8;
        break;
    case 6: {
        int a = body[0];
        int n = snprintf(out + 2, sizeof out - 3, "%d\x1f%ld", a, s->custom[a]);
        out[0] = 0x37; out[1] = 0x27;
        out[2 + n] = 0;
        reply(s, out, (size_t)n + 3);
        break;
    }
    }
}

static void *run(void *arg)
{
    tm_sim *s = arg;
    uint8_t c;
    while (read(s->fd, &c, 1) == 1) {
        if (c == 0x10) {                                   /* DLE EOT n */
            uint8_t b[2];
            if (rd(s, b, 2)) break;
            if (b[0] == 0x04 && b[1] >= 1 && b[1] <= 4)
                reply(s, &s->status[b[1]], 1);
        } else if (c == 0x1D) {
            uint8_t b;
            if (rd(s, &b, 1)) break;
            if (b == 0x49) {                               /* GS I n */
                uint8_t n;
                if (rd(s, &n, 1)) break;
                if (n >= 1 && n <= 3) {
                    /* bare single byte: indistinguishable from noise, so the
                     * simulator never pads it (real printers only send ASB
                     * when GS a enables it) */
                    uint8_t v = (uint8_t)(0x20 + n);
                    (void)!write(s->fd, &v, 1);
                } else {
                    const char *t = n == 0x42 ? "EPSON" : n == 0x43 ? "TM-T20III" :
                                    n == 0x44 ? "X5ER123456" : n == 0x41 ? "1.00 ESC/POS" : "ANK";
                    char o[64];
                    o[0] = 0x5F;
                    size_t tl = strlen(t);
                    memcpy(o + 1, t, tl);
                    o[1 + tl] = 0;
                    reply(s, o, tl + 2);
                }
            } else if (b == 0x28) {
                uint8_t f;
                if (rd(s, &f, 1)) break;
                if (f == 0x45)
                    gs_e(s);
            }
        } else {
            s->other_bytes++;
        }
    }
    return NULL;
}

int tm_sim_start(tm_sim *s, int fd)
{
    memset(s->custom, 0, sizeof s->custom);
    s->fd = fd;
    s->status[1] = 0x16;     /* online */
    s->status[2] = 0x12;
    s->status[3] = 0x12;
    s->status[4] = 0x12;
    pthread_t t;
    if (pthread_create(&t, NULL, run, s))
        return -1;
    pthread_detach(t);
    return 0;
}
