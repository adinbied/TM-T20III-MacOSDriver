/*
 * tmcore_test.c - protocol tests against the simulator (Linux/macOS).
 */
#include "tmcore.h"
#include "tmsim.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static tm_sim sim;

static void apply_preset(tm_link *l, const char *name)
{
    const tm_preset *p = tm_preset_find(name);
    assert(p);
    tm_assign v[64];
    int n = 0;
    for (const char *const *it = p->items; *it; it++) {
        char key[64];
        const char *eq = strchr(*it, '=');
        assert(eq && (size_t)(eq - *it) < sizeof key);
        memcpy(key, *it, (size_t)(eq - *it));
        key[eq - *it] = 0;
        v[n].s = tm_setting_find(key);
        assert(v[n].s);
        assert(tm_setting_parse(v[n].s, eq + 1, &v[n].value) == TM_OK);
        n++;
    }
    assert(tm_apply(l, v, n) == TM_OK);
}

int main(void)
{
    /* catalog / parsing */
    const tm_setting *w = tm_setting_find("paper_width_columns"), *sp = tm_setting_find("speed");
    long v;
    assert(w && sp);
    assert(tm_setting_parse(w, "80mm - 48 columns", &v) == 0 && v == 6);
    assert(tm_setting_parse(w, "58mm - 42", &v) == 0 && v == 0x10002);
    assert(tm_setting_parse(w, "80mm", &v) != 0);                    /* ambiguous */
    assert(tm_setting_parse(w, "0x10006", &v) == 0 && v == 0x10006);
    assert(tm_setting_parse(w, "70", &v) != 0);
    assert(tm_setting_parse(sp, "Level 1", &v) == 0 && v == 1);     /* not Level 10 */
    assert(tm_setting_parse(sp, "level 13", &v) == 0 && v == 13);
    assert(tm_setting_parse(sp, "14", &v) != 0);
    assert(tm_setting_parse(tm_setting_find("resize_ratio"), "100", &v) == 0 && v == 100);
    assert(tm_setting_parse(tm_setting_find("resize_ratio"), "500", &v) != 0);
    puts("catalog: ok");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    sim.noise = 1;
    sim.custom[3] = 2;          /* 58 mm             */
    sim.custom[11] = 1;         /* 42 columns        */
    sim.custom[5] = 3;          /* density level 3   */
    sim.custom[6] = 13;         /* speed 13          */
    sim.custom[101] = 1;        /* reduction enabled */
    sim.msw[1] = 0x9F;          /* bits 8,5,4,3,2,1  */
    assert(tm_sim_start(&sim, sv[1]) == 0);
    sim.custom[5] = 3; sim.custom[6] = 13; sim.custom[101] = 1;   /* start() zeroes */
    sim.custom[3] = 2; sim.custom[11] = 1;

    tm_link l;
    assert(tm_link_fd(&l, sv[0], sv[0]) == TM_OK);

    char info[64];
    assert(tm_info(&l, 0x43, info, sizeof info) == TM_OK && !strcmp(info, "TM-T20III"));
    assert(tm_info(&l, 0x44, info, sizeof info) == TM_OK && !strcmp(info, "X5ER123456"));
    assert(tm_info(&l, 1, info, sizeof info) == TM_OK && !strcmp(info, "33"));

    tm_status_t st;
    assert(tm_status_all(&l, &st) == TM_OK);
    assert(st.online && !st.cover_open && !st.paper_end && !st.near_end);
    sim.status[2] = 0x12 | 0x04;                                   /* cover open */
    sim.status[4] = 0x12 | 0x60;                                   /* paper end  */
    assert(tm_status_all(&l, &st) == TM_OK && st.cover_open && st.paper_end);
    puts("status/info: ok");

    assert(tm_setting_read(&l, tm_setting_find("density"), &v) == TM_OK && v == 3);
    assert(tm_setting_read(&l, w, &v) == TM_OK && v == 0x10002);    /* 58mm - 42 columns */
    assert(tm_setting_read(&l, tm_setting_find("reduce_top"), &v) == TM_OK && v == 1);
    assert(tm_setting_read(&l, tm_setting_find("auto_lf"), &v) == TM_OK && v == 1);
    assert(tm_setting_read(&l, tm_setting_find("rx_buffer"), &v) == TM_OK && v == 1);
    puts("read: ok");

    apply_preset(&l, "truss");
    uint8_t sync;
    assert(tm_status(&l, 1, &sync) == TM_OK);       /* sim has processed OUT */
    assert(sim.resets == 1 && sim.ignored_writes == 0 && !sim.in_setting);
    assert(sim.custom[3] == 6 && sim.custom[11] == 0);
    assert(sim.custom[5] == 4 && sim.custom[6] == 10);
    assert(sim.custom[70] == 0 && sim.custom[71] == 1 && sim.custom[73] == 100);
    assert(sim.custom[98] == 2 && sim.custom[100] == 1);
    for (int a = 101; a <= 106; a++)
        assert(sim.custom[a] == 0);
    assert(sim.msw[1] == 0x81);            /* bits 2-5 cleared, 8 and 1 kept */
    assert(sim.custom[1] == 0 && sim.custom[2] == 0);              /* NV untouched */

    /* read back through the protocol */
    assert(tm_setting_read(&l, tm_setting_find("speed"), &v) == TM_OK && v == 10);
    assert(tm_setting_read(&l, tm_setting_find("busy_condition"), &v) == TM_OK && v == 0);
    puts("apply truss preset: ok");

    /* guard rails */
    tm_assign bad = {&(tm_setting){"nv", "nv", "x", TM_KIND_CUSTOM, 1, 0, 1, 0, 0, NULL}, 0};
    assert(tm_apply(&l, &bad, 1) == TM_ERR_ARG);
    assert(sim.resets == 1);
    puts("guard rails: ok");

    tm_link_close(&l);
    puts("ALL OK");
    return 0;
}
