/*
 * tmqueue - Printing Preferences for a TM-T20III CUPS queue, from the shell.
 *
 *   tmqueue keys                                   list settings + defaults
 *   tmqueue show  QUEUE|file.ppd                   current settings + papers
 *   tmqueue set   QUEUE|file.ppd key=value ...     change settings
 *   tmqueue paper-add QUEUE|file.ppd NAME W H [L T R B] [--inch]
 *   tmqueue paper-del QUEUE|file.ppd NAME
 *
 * Options: -t template.ppd  (default: installed EpsonTMT20III.ppd)
 *          -o out.ppd       write the generated PPD instead of applying it
 *
 * For a queue, the new PPD is applied with "lpadmin -p QUEUE -P file"
 * (run with sudo if lpadmin asks for authentication).  TMUtility.app's
 * Printing Preferences window does the same thing.
 */
#include "tmdriver.h"

#include <cups/cups.h>
#include <cups/ppd.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_TEMPLATE "/Library/Printers/PPDs/Contents/Resources/EpsonTMT20III.ppd"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    if (s)
        s[n] = 0;
    fclose(f);
    return s;
}

static int is_file(const char *t) { return strchr(t, '/') || strstr(t, ".ppd"); }

/* Queue name or file -> PPD text. */
static char *load_target(const char *target)
{
    if (is_file(target))
        return slurp(target);
    const char *tmp = cupsGetPPD(target);
    if (!tmp) {
        fprintf(stderr, "tmqueue: cannot get the PPD of queue '%s': %s\n", target, cupsLastErrorString());
        return NULL;
    }
    char *s = slurp(tmp);
    unlink(tmp);
    return s;
}

static void show(const tm_drv *d)
{
    for (int i = 0; tm_drv_desc_at(i); i++) {
        const tm_drv_desc *s = tm_drv_desc_at(i);
        printf("%-24s %s%s\n", s->key, tm_drv_get(d, s->key), tm_drv_is_default(d, s->key) ? "" : "   *");
    }
    for (int i = 0; tm_drv_paper_at(d, i); i++) {
        const tm_drv_paper *p = tm_drv_paper_at(d, i);
        double k = p->unit_inch ? 25400.0 : 1000.0;
        const char *u = p->unit_inch ? "in" : "mm";
        printf("paper %-6s %-31s %.2f x %.2f %s  margins L%.2f T%.2f R%.2f B%.2f\n", p->keyword, p->name,
               p->width_um / k, p->height_um / k, u, p->left_um / k, p->top_um / k, p->right_um / k,
               p->bottom_um / k);
    }
}

static int apply(const char *target, const char *out, const char *tmpl_path, const tm_drv *d)
{
    char *tmpl = slurp(tmpl_path);
    if (!tmpl) {
        fprintf(stderr, "tmqueue: cannot read template %s (use -t)\n", tmpl_path);
        return 1;
    }
    char *ppd = tm_drv_generate_ppd(tmpl, d);
    free(tmpl);
    if (!ppd) {
        fprintf(stderr, "tmqueue: %s is not the rastertotmt20 template (no @@ markers)\n", tmpl_path);
        return 1;
    }
    char path[256];
    int rc = 0;
    if (out) {
        snprintf(path, sizeof path, "%s", out);
    } else if (is_file(target)) {
        fputs(ppd, stdout);
        free(ppd);
        return 0;
    } else {
        snprintf(path, sizeof path, "/tmp/tmqueue-XXXXXX");
        int fd = mkstemp(path);
        if (fd < 0) { perror("mkstemp"); free(ppd); return 1; }
        close(fd);
    }
    FILE *f = fopen(path, "w");
    if (!f || fputs(ppd, f) < 0 || fclose(f)) {
        perror(path);
        free(ppd);
        return 1;
    }
    free(ppd);
    if (!out) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/usr/sbin/lpadmin", "lpadmin", "-p", target, "-P", path, (char *)NULL);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        unlink(path);
        if (rc)
            fprintf(stderr, "tmqueue: lpadmin failed (try: sudo tmqueue ...)\n");
        else
            printf("Updated queue %s.\n", target);
    }
    return rc;
}

static long to_um(const char *s, int inch)
{
    return (long)(strtod(s, NULL) * (inch ? 25400.0 : 1000.0) + 0.5);
}

static int usage(void)
{
    fputs("usage: tmqueue [-t template.ppd] [-o out.ppd] keys | show T | set T key=value...\n"
          "               | paper-add T NAME WIDTH HEIGHT [LEFT TOP RIGHT BOTTOM] [--inch]\n"
          "               | paper-del T NAME            (T = queue name or .ppd file)\n", stderr);
    return 2;
}

int main(int argc, char **argv)
{
    const char *tmpl = getenv("TM_PPD_TEMPLATE") ? getenv("TM_PPD_TEMPLATE") : DEFAULT_TEMPLATE;
    const char *out = NULL;
    int c;
    while ((c = getopt(argc, argv, "t:o:")) != -1) {
        if (c == 't') tmpl = optarg;
        else if (c == 'o') out = optarg;
        else return usage();
    }
    argc -= optind;
    argv += optind;
    if (argc < 1)
        return usage();

    if (!strcmp(argv[0], "keys")) {
        for (int i = 0; tm_drv_desc_at(i); i++) {
            const tm_drv_desc *s = tm_drv_desc_at(i);
            printf("%-24s default %-12s %s\n", s->key, *s->def ? s->def : "\"\"",
                   s->choices ? s->choices : s->min > s->max ? "(text)" : "");
        }
        return 0;
    }
    if (argc < 2)
        return usage();
    char *text = load_target(argv[1]);
    if (!text) {
        if (is_file(argv[1]))
            perror(argv[1]);
        return 1;
    }
    tm_drv d;
    tm_drv_parse_ppd(&d, text);
    free(text);

    if (!strcmp(argv[0], "show")) {
        show(&d);
        return 0;
    }
    if (!strcmp(argv[0], "set")) {
        for (int i = 2; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) return usage();
            *eq = 0;
            int rc = tm_drv_set(&d, argv[i], eq + 1);
            if (rc) {
                fprintf(stderr, "tmqueue: %s=%s: %s\n", argv[i], eq + 1, tm_drv_strerror(rc));
                return 1;
            }
        }
        return apply(argv[1], out, tmpl, &d);
    }
    if (!strcmp(argv[0], "paper-add")) {
        int inch = 0, n = 0;
        const char *a[8];
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--inch")) inch = 1;
            else if (n < 7) a[n++] = argv[i];
            else return usage();
        }
        if (n != 3 && n != 7)
            return usage();
        tm_drv_paper p;
        memset(&p, 0, sizeof p);
        snprintf(p.name, sizeof p.name, "%s", a[0]);
        p.width_um = to_um(a[1], inch);
        p.height_um = to_um(a[2], inch);
        if (n == 7) {
            p.left_um = to_um(a[3], inch);
            p.top_um = to_um(a[4], inch);
            p.right_um = to_um(a[5], inch);
            p.bottom_um = to_um(a[6], inch);
        }
        p.unit_inch = inch;
        int rc = tm_drv_paper_add(&d, &p);
        if (rc) {
            fprintf(stderr, "tmqueue: %s\n", tm_drv_strerror(rc));
            return 1;
        }
        return apply(argv[1], out, tmpl, &d);
    }
    if (!strcmp(argv[0], "paper-del") && argc == 3) {
        int rc = tm_drv_paper_del(&d, argv[2]);
        if (rc) {
            fprintf(stderr, "tmqueue: %s: %s\n", argv[2], tm_drv_strerror(rc));
            return 1;
        }
        return apply(argv[1], out, tmpl, &d);
    }
    return usage();
}
