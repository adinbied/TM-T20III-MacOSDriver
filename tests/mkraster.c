/*
 * mkraster - write a CUPS raster test job (1-bit, K colour space, 203 dpi).
 *
 *   mkraster out.ras W_PT H_PT IMG_LLX IMG_URX PAGES
 *
 * Each page holds a test pattern defined in page coordinates (dots):
 *   - a 1-dot frame around the whole page
 *   - a solid 16x16 block whose top-left corner is at (20, 10)
 *   - a horizontal line every 100 rows
 * tests/filter_test.py rebuilds the same pattern to check the filter output.
 */
#include <cups/raster.h>

#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ink(int x, int y, int w, int h)
{
    if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
        return 1;
    if (x >= 20 && x < 36 && y >= 10 && y < 26)
        return 1;
    return y % 100 == 50;
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: %s out.ras W_PT H_PT IMG_LLX IMG_URX PAGES\n", argv[0]);
        return 2;
    }
    double wpt = atof(argv[2]), hpt = atof(argv[3]);
    double llx = atof(argv[4]), urx = atof(argv[5]);
    int pages = atoi(argv[6]);
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cups_raster_t *r = fd >= 0 ? cupsRasterOpen(fd, CUPS_RASTER_WRITE) : NULL;
    if (!r) {
        perror("open");
        return 1;
    }
    cups_page_header2_t h;
    memset(&h, 0, sizeof h);
    h.HWResolution[0] = h.HWResolution[1] = 203;
    h.PageSize[0] = (unsigned)lround(wpt);
    h.PageSize[1] = (unsigned)lround(hpt);
    h.ImagingBoundingBox[0] = (unsigned)lround(llx);
    h.ImagingBoundingBox[2] = (unsigned)lround(urx);
    h.ImagingBoundingBox[3] = (unsigned)lround(hpt);
    h.cupsPageSize[0] = (float)wpt;               /* exact values, as cgpdftoraster */
    h.cupsPageSize[1] = (float)hpt;
    h.cupsImagingBBox[0] = (float)llx;
    h.cupsImagingBBox[2] = (float)urx;
    h.cupsImagingBBox[3] = (float)hpt;
    h.cupsWidth = (unsigned)lround(wpt * 203 / 72.0);
    h.cupsHeight = (unsigned)lround(hpt * 203 / 72.0);
    h.cupsBitsPerColor = 1;
    h.cupsBitsPerPixel = 1;
    h.cupsBytesPerLine = (h.cupsWidth + 7) / 8;
    h.cupsColorSpace = CUPS_CSPACE_K;
    h.cupsColorOrder = CUPS_ORDER_CHUNKED;
    h.NumCopies = 1;
    unsigned char *line = malloc(h.cupsBytesPerLine);
    for (int p = 0; p < pages; p++) {
        cupsRasterWriteHeader2(r, &h);
        for (unsigned y = 0; y < h.cupsHeight; y++) {
            memset(line, 0, h.cupsBytesPerLine);
            for (unsigned x = 0; x < h.cupsWidth; x++)
                if (ink((int)x, (int)y, (int)h.cupsWidth, (int)h.cupsHeight))
                    line[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
            cupsRasterWritePixels(r, line, h.cupsBytesPerLine);
        }
    }
    cupsRasterClose(r);
    close(fd);
    printf("%u %u\n", h.cupsWidth, h.cupsHeight);
    return 0;
}
