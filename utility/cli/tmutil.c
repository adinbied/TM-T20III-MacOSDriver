/*
 * tmutil - TM-T20III settings utility (command line).
 *
 *   tmutil [--tcp host[:port]] <command> [args]
 *
 *   info                    model, serial, firmware
 *   status                  online / cover / paper / errors
 *   list                    every known setting and its possible values
 *   get [key ...]           read settings (all if none given)
 *   dump                    raw customize values a=1..255 that answer
 *   set key=value ...       write settings (printer resets afterwards)
 *   preset <name>           apply a preset (truss, noreduce)
 *   backup <file>           save all settings as key=value lines
 *   restore <file>          write settings from a backup file
 *   test-print [text]       print a short test receipt and cut
 *   drawer <2|5>            kick cash drawer
 *   buzz <pattern> [count]  sound the optional buzzer
 *
 * Writes show the current -> new values and ask for confirmation unless -y.
 * Default transport: USB (macOS).  Close this tool before printing.
 */
#include "tmcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int assume_yes = 0;

static int die(const char *what, int rc)
{
    fprintf(stderr, "tmutil: %s: %s\n", what, tm_strerror(rc));
    return 1;
}

static void print_value(const tm_setting *s, long v)
{
    const char *lbl = tm_setting_label(s, v);
    if (lbl)
        printf("  %-20s %-38s %s (%ld)%s\n", s->key, s->title, lbl, v, s->verified ? "" : "  [unverified]");
    else
        printf("  %-20s %-38s %ld%s\n", s->key, s->title, v, s->verified ? "" : "  [unverified]");
}

static int cmd_get(tm_link *l, int argc, char **argv)
{
    const char *group = NULL;
    for (int i = 0; i < tm_settings_count; i++) {
        const tm_setting *s = &tm_settings[i];
        if (argc) {
            int want = 0;
            for (int k = 0; k < argc; k++)
                want |= !strcasecmp(argv[k], s->key);
            if (!want)
                continue;
        }
        if (!group || strcmp(group, s->group)) {
            group = s->group;
            printf("%s\n", group);
        }
        long v;
        int rc = tm_setting_read(l, s, &v);
        if (rc)
            printf("  %-20s %-38s <%s>\n", s->key, s->title, tm_strerror(rc));
        else
            print_value(s, v);
    }
    return 0;
}

static int confirm(void)
{
    if (assume_yes)
        return 1;
    printf("Write these settings? The printer will reset. [y/N] ");
    fflush(stdout);
    char b[16];
    return fgets(b, sizeof b, stdin) && (b[0] == 'y' || b[0] == 'Y');
}

/* Parse key=value items, show diff against printer, apply. */
static int write_items(tm_link *l, int n, const char *const *items)
{
    tm_assign v[64];
    int k = 0;
    for (int i = 0; i < n; i++) {
        char key[64];
        const char *eq = strchr(items[i], '=');
        if (!eq || eq == items[i] || (size_t)(eq - items[i]) >= sizeof key) {
            fprintf(stderr, "tmutil: expected key=value, got '%s'\n", items[i]);
            return 1;
        }
        memcpy(key, items[i], (size_t)(eq - items[i]));
        key[eq - items[i]] = 0;
        const tm_setting *s = tm_setting_find(key);
        if (!s) {
            fprintf(stderr, "tmutil: unknown setting '%s' (see 'tmutil list')\n", key);
            return 1;
        }
        long val;
        if (tm_setting_parse(s, eq + 1, &val)) {
            fprintf(stderr, "tmutil: bad value '%s' for %s\n", eq + 1, key);
            return 1;
        }
        if (k == 64)
            return 1;
        v[k].s = s;
        v[k++].value = val;
    }

    int changes = 0;
    printf("%-20s %-28s    %s\n", "setting", "current", "new");
    for (int i = 0; i < k; i++) {
        long cur;
        int rc = tm_setting_read(l, v[i].s, &cur);
        const char *cl = rc ? "?" : tm_setting_label(v[i].s, cur);
        const char *nl = tm_setting_label(v[i].s, v[i].value);
        char cb[32], nb[32];
        if (!cl) { snprintf(cb, sizeof cb, "%ld", cur); cl = cb; }
        if (!nl) { snprintf(nb, sizeof nb, "%ld", v[i].value); nl = nb; }
        int same = !rc && cur == v[i].value;
        changes += !same;
        printf("%-20s %-28s %s %s%s\n", v[i].s->key, cl, same ? "  " : "->", nl,
               v[i].s->verified ? "" : "  [unverified mapping]");
    }
    if (!changes) {
        puts("Nothing to change.");
        return 0;
    }
    if (!confirm())
        return 1;
    int rc = tm_apply(l, v, k);
    if (rc)
        return die("apply", rc);
    puts("Written. The printer is resetting; wait a few seconds before printing.");
    return 0;
}

static int cmd_backup(tm_link *l, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    fprintf(f, "# TM-T20III settings backup (tmutil)\n");
    for (int i = 0; i < tm_settings_count; i++) {
        long v;
        if (tm_setting_read(l, &tm_settings[i], &v) == TM_OK)
            fprintf(f, "%s=%ld\n", tm_settings[i].key, v);
        else
            fprintf(f, "# %s=<no answer>\n", tm_settings[i].key);
    }
    fclose(f);
    printf("Saved %s\n", path);
    return 0;
}

static int cmd_restore(tm_link *l, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    static char lines[64][96];
    const char *items[64];
    int n = 0;
    while (n < 64 && fgets(lines[n], sizeof lines[n], f)) {
        char *p = lines[n];
        p[strcspn(p, "\r\n")] = 0;
        if (!*p || *p == '#')
            continue;
        items[n] = p;
        n++;
    }
    fclose(f);
    return write_items(l, n, items);
}

static int cmd_dump(tm_link *l)
{
    puts("a    dec  value   known as");
    for (int a = 1; a <= 255; a++) {
        long v;
        if (tm_customize_get(l, a, &v) != TM_OK)
            continue;
        const char *key = "";
        for (int i = 0; i < tm_settings_count; i++)
            if ((tm_settings[i].kind != TM_KIND_MSW && tm_settings[i].id == a) ||
                (tm_settings[i].kind == TM_KIND_PAIR && tm_settings[i].bit == a))
                key = tm_settings[i].key;
        printf("0x%02X %3d  %-6ld  %s\n", a, a, v, key);
    }
    for (int m = 1; m <= 8; m++) {
        uint8_t b;
        if (tm_msw_get(l, m, &b) == TM_OK) {
            printf("Msw%d  ", m);
            for (int k = 7; k >= 0; k--)
                putchar(b & (1 << k) ? '1' : '0');
            puts("  (bit8..bit1)");
        }
    }
    return 0;
}

static void usage(void)
{
    fputs("usage: tmutil [--tcp host[:port]] [-y] <info|status|list|get|dump|set|preset|"
          "backup|restore|test-print|drawer|buzz> [args]\n", stderr);
}

int main(int argc, char **argv)
{
    const char *tcp = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--tcp") && i + 1 < argc)
            tcp = argv[++i];
        else if (!strcmp(argv[i], "-y"))
            assume_yes = 1;
        else { usage(); return 2; }
    }
    if (i >= argc) { usage(); return 2; }
    const char *cmd = argv[i++];
    int nargs = argc - i;
    char **args = argv + i;

    if (!strcmp(cmd, "list")) {
        const char *group = NULL;
        for (int k = 0; k < tm_settings_count; k++) {
            const tm_setting *s = &tm_settings[k];
            if (!group || strcmp(group, s->group))
                printf("%s\n", group = s->group);
            printf("  %-20s %s%s\n", s->key, s->title, s->verified ? "" : "  [unverified]");
            if (s->choices)
                for (const tm_choice *c = s->choices; c->label; c++)
                    printf("      %-6ld %s\n", c->value, c->label);
            else
                printf("      %ld..%ld\n", s->min, s->max);
        }
        puts("Presets");
        for (const tm_preset *p = tm_presets; p->name; p++)
            printf("  %-20s %s\n", p->name, p->title);
        return 0;
    }

    tm_link l;
    int rc;
    if (tcp) {
        char host[256];
        int port = 9100;
        snprintf(host, sizeof host, "%s", tcp);
        char *c = strrchr(host, ':');
        if (c) { *c = 0; port = atoi(c + 1); }
        rc = tm_link_tcp(&l, host, port);
    } else {
        rc = tm_link_usb(&l);
    }
    if (rc)
        return die(tcp ? tcp : "USB", rc);

    int ret = 0;
    if (!strcmp(cmd, "info")) {
        static const struct { int n; const char *name; } q[] = {
            {0x42, "Maker"}, {0x43, "Model"}, {0x44, "Serial"}, {0x41, "Firmware"}, {0x45, "Font"}};
        for (size_t k = 0; k < sizeof q / sizeof q[0]; k++) {
            char b[128];
            rc = tm_info(&l, q[k].n, b, sizeof b);
            printf("%-9s %s\n", q[k].name, rc ? tm_strerror(rc) : b);
        }
    } else if (!strcmp(cmd, "status")) {
        tm_status_t s;
        if ((rc = tm_status_all(&l, &s)))
            ret = die("status", rc);
        else
            printf("%s%s%s%s%s%s\n", s.online ? "online" : "OFFLINE",
                   s.cover_open ? ", cover open" : "", s.paper_end ? ", PAPER OUT" : "",
                   s.near_end ? ", paper near end" : "", s.cutter_error ? ", CUTTER ERROR" : "",
                   s.unrecoverable ? ", UNRECOVERABLE ERROR" : "");
    } else if (!strcmp(cmd, "get")) {
        ret = cmd_get(&l, nargs, args);
    } else if (!strcmp(cmd, "dump")) {
        ret = cmd_dump(&l);
    } else if (!strcmp(cmd, "set") && nargs) {
        ret = write_items(&l, nargs, (const char *const *)args);
    } else if (!strcmp(cmd, "preset") && nargs == 1) {
        const tm_preset *p = tm_preset_find(args[0]);
        if (!p) { fprintf(stderr, "tmutil: unknown preset '%s'\n", args[0]); ret = 1; }
        else {
            int n = 0;
            while (p->items[n]) n++;
            printf("Preset: %s\n", p->title);
            ret = write_items(&l, n, p->items);
        }
    } else if (!strcmp(cmd, "backup") && nargs == 1) {
        ret = cmd_backup(&l, args[0]);
    } else if (!strcmp(cmd, "restore") && nargs == 1) {
        ret = cmd_restore(&l, args[0]);
    } else if (!strcmp(cmd, "test-print")) {
        rc = tm_print_test(&l, nargs ? args[0] : "TM-T20III test print\nmacOS tmutil\n");
        if (rc) ret = die("test-print", rc);
    } else if (!strcmp(cmd, "drawer") && nargs == 1) {
        if ((rc = tm_kick_drawer(&l, atoi(args[0])))) ret = die("drawer", rc);
    } else if (!strcmp(cmd, "buzz") && nargs >= 1) {
        if ((rc = tm_buzz(&l, atoi(args[0]), nargs > 1 ? atoi(args[1]) : 1))) ret = die("buzz", rc);
    } else {
        usage();
        ret = 2;
    }
    tm_link_close(&l);
    return ret;
}
