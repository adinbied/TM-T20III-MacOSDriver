/*
 * tmdriver_test.c - Printing Preferences model / PPD generator tests.
 *   build/tmdriver_test ../ppd/EpsonTMT20III.ppd
 */
#include "tmdriver.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    assert(fread(s, 1, (size_t)n, f) == (size_t)n);
    s[n] = 0;
    fclose(f);
    return s;
}

static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p++)
        n++;
    return n;
}

int main(int argc, char **argv)
{
    char *tmpl = slurp(argc > 1 ? argv[1] : "../ppd/EpsonTMT20III.ppd");
    static tm_drv d, d2, d3;

    /* defaults = template defaults = Windows defaults */
    tm_drv_parse_ppd(&d, tmpl);
    assert(!strcmp(tm_drv_get(&d, "TmFeedCut"), "DocFeedCut"));
    assert(!strcmp(tm_drv_get(&d, "PageSize"), "RP80x297"));
    assert(!strcmp(tm_drv_get(&d, "tm-cut-docend"), "17"));
    assert(!strcmp(tm_drv_get(&d, "tm-unsupported-auto"), "1"));
    assert(d.npapers == 0);

    /* validation */
    assert(tm_drv_set(&d, "TmFeedCut", "Bogus") == TM_DRV_EVALUE);
    assert(tm_drv_set(&d, "nope", "1") == TM_DRV_EKEY);
    assert(tm_drv_set(&d, "tm-dk-docend", "4") == TM_DRV_EVALUE);
    assert(tm_drv_set(&d, "tm-dk-count-docend", "256") == TM_DRV_EVALUE);
    assert(tm_drv_set(&d, "tm-margin-left", "11001") == TM_DRV_EVALUE);
    assert(tm_drv_set(&d, "tm-cmd-docend", "1B 4") == TM_DRV_EVALUE);
    assert(tm_drv_set(&d, "PageSize", "Custom.80x100mm") == TM_DRV_EVALUE);
    assert(tm_drv_set(&d, "PageSize", "bad name") == TM_DRV_EVALUE);
    puts("validation: ok");

    /* edits */
    assert(tm_drv_set(&d, "TmFeedCut", "Detail") == 0);
    assert(tm_drv_set(&d, "tm-cut-pageend", "1") == 0);
    assert(tm_drv_set(&d, "tm-feed-docend", "12500") == 0);
    assert(tm_drv_set(&d, "tm-dk-docend", "257") == 0);
    assert(tm_drv_set(&d, "tm-cmd-docbegin", "1b40 \"Hi\"") == 0);
    assert(!strcmp(tm_drv_get(&d, "tm-cmd-docbegin"), "1B 40 48 69"));   /* normalised */
    assert(tm_drv_set(&d, "TmPaperSource", "Bottom") == 0);
    assert(tm_drv_set(&d, "tm-margin-top-enable", "1") == 0);
    assert(tm_drv_set(&d, "tm-margin-top", "2500") == 0);

    /* user defined paper: Paperwork.show's TRUSS TAPE 3.18 x 12 in */
    tm_drv_paper p = {0};
    strcpy(p.name, "TRUSS TAPE");
    p.width_um = 80772;
    p.height_um = 304800;
    p.unit_inch = 1;
    assert(tm_drv_paper_add(&d, &p) == 0 && !strcmp(d.paper[0].keyword, "UDP1"));
    tm_drv_paper bad = p;
    bad.width_um = 50000;
    assert(tm_drv_paper_add(&d, &bad) == TM_DRV_ERANGE);
    bad = p;
    bad.left_um = 40000; bad.right_um = 41000;
    assert(tm_drv_paper_add(&d, &bad) == TM_DRV_EMARGIN);
    bad = p;
    strcpy(bad.name, "a:b");
    assert(tm_drv_paper_add(&d, &bad) == TM_DRV_ENAME);
    tm_drv_paper q = p;
    strcpy(q.name, "Label 58x40");
    q.width_um = 58000; q.height_um = 40000; q.left_um = 2000; q.top_um = 1000; q.unit_inch = 0;
    assert(tm_drv_paper_add(&d, &q) == 0 && !strcmp(d.paper[1].keyword, "UDP2"));
    p.height_um = 609600;                                   /* update keeps UDP1 */
    assert(tm_drv_paper_add(&d, &p) == 0 && d.npapers == 2 && !strcmp(d.paper[0].keyword, "UDP1"));
    assert(tm_drv_set(&d, "PageSize", "UDP1") == 0);
    puts("papers: ok");

    /* generate -> parse round trip */
    char *ppd = tm_drv_generate_ppd(tmpl, &d);
    assert(ppd);
    assert(strstr(ppd, "*DefaultTmFeedCut: Detail\n"));
    assert(strstr(ppd, "*DefaultPageSize: UDP1\n") && strstr(ppd, "*DefaultPageRegion: UDP1\n"));
    assert(strstr(ppd, "*DefaultImageableArea: UDP1\n") && strstr(ppd, "*DefaultPaperDimension: UDP1\n"));
    assert(strstr(ppd, "*TmSetting tm-cut-pageend: \"1\"\n"));
    assert(strstr(ppd, "*TmSetting tm-cmd-docbegin: \"1B 40 48 69\"\n"));
    assert(!strstr(ppd, "*TmSetting tm-cut-docend"));                    /* default: omitted */
    assert(strstr(ppd, "*PageSize UDP1/TRUSS TAPE: \"<</PageSize[228.96 1728.00]/ImagingBBox null>>setpagedevice\"\n"));
    assert(strstr(ppd, "*ImageableArea UDP2/Label 58x40: \"5.67 0.00 164.41 110.55\"\n"));
    assert(strstr(ppd, "*TmUserPaper UDP1/TRUSS TAPE: \"80772 609600 0 0 0 0 1\"\n"));
    tm_drv_parse_ppd(&d2, ppd);
    for (int i = 0; tm_drv_desc_at(i); i++) {
        const char *k = tm_drv_desc_at(i)->key;
        assert(!strcmp(tm_drv_get(&d, k), tm_drv_get(&d2, k)));
    }
    assert(d2.npapers == 2);
    for (int i = 0; i < 2; i++) {
        const tm_drv_paper *a = &d.paper[i], *b = &d2.paper[i];
        assert(!strcmp(a->keyword, b->keyword) && !strcmp(a->name, b->name));
        assert(a->width_um == b->width_um && a->height_um == b->height_um && a->unit_inch == b->unit_inch);
        assert(a->left_um == b->left_um && a->top_um == b->top_um);
        assert(a->right_um == b->right_um && a->bottom_um == b->bottom_um);
    }

    /* regenerating from the generated PPD (re-used queue PPD) is stable */
    char *again = tm_drv_generate_ppd(tmpl, &d2);
    assert(again && !strcmp(again, ppd));
    assert(count(ppd, "*PageSize UDP1/") == 1);

    /* deleting the default paper resets PageSize */
    assert(tm_drv_paper_del(&d2, "truss tape") == 0);
    assert(!strcmp(tm_drv_get(&d2, "PageSize"), "RP80x297"));
    char *after = tm_drv_generate_ppd(ppd, &d2);       /* generated PPD as template */
    tm_drv_parse_ppd(&d3, after);
    assert(d3.npapers == 1 && !strcmp(d3.paper[0].name, "Label 58x40"));
    assert(count(after, "UDP1") == 0);
    puts("generate/parse: ok");

    FILE *f = fopen("build/generated.ppd", "w");
    if (f) { fputs(ppd, f); fclose(f); }
    free(ppd); free(again); free(after); free(tmpl);
    puts("ALL OK");
    return 0;
}
