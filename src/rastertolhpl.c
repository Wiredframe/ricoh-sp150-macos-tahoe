/*
 * rastertolhpl - CUPS filter: CUPS raster -> RICOH SP 150 "LHPL" print stream.
 *
 * Native (arm64/x86_64) replacement for Ricoh's proprietary x86_64-only
 * RICOH_SP_150Filter.app, which stopped running on Apple Silicon once Rosetta 2
 * was removed. The wire format was reproduced from the vendor filter's output:
 *
 *   ESC%-12345X@PJL ... @PJL ENTER LANGUAGE=LHPL
 *   ESC LH@sj <57 bytes> <xor>          job start (constant)
 *   per page:
 *     ESC LH@sp <57 bytes> <xor>        page header (paper code, size, lengths)
 *     <JBIG1 BIE>                       1 plane, L0=128, LRLTWO, SDRST, mx=0
 *     ESC LH@ep <57 bytes> <xor>        page end (vendor leaves this uninitialised)
 *   ESC%-12345X@PJL EOJ
 *
 * <xor> is the XOR of every byte of the block including the 6-byte "ESC LH@xx"
 * prefix. Like the vendor filter, pages are sent in reverse order (last page
 * first) unless the job options contain OutputOrder=Normal. Gray is turned into 1 bit with the driver's own 32x32 ordered-dither
 * matrix (lhpl_screen.h), so output is byte-identical to the original driver.
 *
 * Input: 8-bit gray CUPS raster (cupsColorSpace W or K), as produced by
 * pdftoraster_cg. CUPS invokes: filter job user title copies options [file]
 *
 * SPDX-License-Identifier: MIT (this file). Links against jbigkit (GPL-2.0+).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <cups/raster.h>
#include "jbig/jbig.h"
#include "lhpl_screen.h"

/* Paper codes as emitted by the vendor filter, matched by PageSize in points. */
static const struct { const char *name; double w, h; unsigned char code; } papers[] = {
    { "Letter",    612, 792,  1 },
    { "A4",        595, 842,  2 },
    { "A5",        420, 595,  3 },
    { "A6",        297, 420,  4 },
    { "B5",        516, 729,  5 },
    { "B6",        363, 516,  6 },
    { "Executive", 522, 756,  7 },
    { "16K",       524, 737,  8 },
    { "Legal",     612, 1008, 9 },
};

typedef struct { unsigned char *data; size_t size, cap; } Buf;

static void buf_cb(unsigned char *d, size_t n, void *arg)
{
    Buf *b = (Buf *)arg;
    if (b->size + n > b->cap) {
        size_t nc = (b->size + n) * 2;
        unsigned char *nd = realloc(b->data, nc);
        if (!nd) { fprintf(stderr, "ERROR: rastertolhpl: out of memory\n"); exit(1); }
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->size, d, n);
    b->size += n;
}

static void put_u32(unsigned char *p, unsigned v)
{ p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }
static void put_u16(unsigned char *p, unsigned v)
{ p[0] = v & 255; p[1] = (v >> 8) & 255; }

/* Append "ESC LH@" + 2-char tag + 57 body bytes + XOR trailer (64 bytes) to buf,
 * or write it to stdout when buf is NULL. */
static void write_block(Buf *buf, const char *tag, const unsigned char body[57])
{
    unsigned char blk[64];
    blk[0] = 0x1b; blk[1] = 'L'; blk[2] = 'H'; blk[3] = '@'; blk[4] = tag[0]; blk[5] = tag[1];
    memcpy(blk + 6, body, 57);
    unsigned char x = 0;
    for (int i = 0; i < 63; i++) x ^= blk[i];
    blk[63] = x;
    if (buf) buf_cb(blk, 64, buf); else fwrite(blk, 1, 64, stdout);
}

/* Keep PJL attribute values printable and short. */
static void sanitize(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; src && *src && i < n - 1; src++)
        if (isprint((unsigned char)*src) && *src != '"' && *src != '\\') dst[i++] = *src;
    dst[i] = 0;
    if (i == 0) strncpy(dst, "document", n);
}

int main(int argc, char **argv)
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }
    int fd = 0;
    if (argc == 7 && argv[6][0]) {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "ERROR: rastertolhpl: cannot open %s\n", argv[6]); return 1; }
    }
    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) { fprintf(stderr, "ERROR: rastertolhpl: cupsRasterOpen failed\n"); return 1; }

    char user[64], title[128], host[64];
    sanitize(user, argv[2], sizeof user);
    sanitize(title, argv[3], sizeof title);
    if (gethostname(host, sizeof host) != 0) strcpy(host, "localhost");
    host[sizeof host - 1] = 0;
    char *dot = strchr(host, '.'); if (dot) *dot = 0;

    int reverse = !(strcasestr(argv[5], "OutputOrder=Normal") != NULL);

    cups_page_header2_t h;
    int page = 0;
    unsigned char *line = NULL, *bmp = NULL;
    Buf jb = { NULL, 0, 0 };
    Buf *pages = NULL;          /* finished pages (sp block + JBIG + ep block) */

    while (cupsRasterReadHeader2(ras, &h)) {
        if (h.cupsBitsPerPixel != 8 || h.cupsBitsPerColor != 8 ||
            (h.cupsColorSpace != CUPS_CSPACE_W && h.cupsColorSpace != CUPS_CSPACE_K &&
             h.cupsColorSpace != CUPS_CSPACE_SW)) {
            fprintf(stderr, "ERROR: rastertolhpl: need 8-bit gray raster (got %u bpp, cspace %u)\n",
                    h.cupsBitsPerPixel, h.cupsColorSpace);
            return 1;
        }
        int invert = (h.cupsColorSpace == CUPS_CSPACE_K);   /* K: 255 = full toner */
        unsigned copies = h.NumCopies ? h.NumCopies : 1;

        if (page == 0) {
            time_t now = time(NULL); struct tm *t = localtime(&now);
            printf("\x1b%%-12345X@PJL JOB NAME=PRINTER\r\n"
                   "@PJL SET JOBATTR=HST:%s\r\n"
                   "@PJL SET JOBATTR=USR:%s\r\n"
                   "@PJL SET JOBATTR=DOC:%s\r\n"
                   "@PJL SET JOBATTR=DATE:%02d/%02d/%04d\r\n"
                   "@PJL SET JOBATTR=TIME:%02d:%02d:%02d\r\n"
                   "@PJL SET DUPLEX=OFF\r\n"
                   "@PJL SET MEDIASOURCE=0\r\n"
                   "@PJL SET RENDERMODE=GRAYSCALE\r\n"
                   "@PJL SET RESOLUTION=600\r\n"
                   "@PJL SET BITSPERPIXEL=1\r\n"
                   "@PJL SET COPIES=%u\r\n"
                   "@PJL ENTER LANGUAGE=LHPL\r\n",
                   host, user, title,
                   t->tm_mon + 1, t->tm_mday, t->tm_year + 1900,
                   t->tm_hour, t->tm_min, t->tm_sec, copies);
            unsigned char sj[57] = { 0x01, 0x00, 0x01, 0x00 };
            write_block(NULL, "sj", sj);
        }

        /* Paper code: nearest entry by PageSize (points). */
        unsigned char code = 2; double best = 1e9; unsigned pi = 1;
        for (unsigned i = 0; i < sizeof papers / sizeof papers[0]; i++) {
            double d = (papers[i].w - h.PageSize[0]) * (papers[i].w - h.PageSize[0]) +
                       (papers[i].h - h.PageSize[1]) * (papers[i].h - h.PageSize[1]);
            if (d < best) { best = d; code = papers[i].code; pi = i; }
        }
        if (best > 25) fprintf(stderr, "WARNING: rastertolhpl: PageSize %ux%u pt not in table, using %s\n",
                               h.PageSize[0], h.PageSize[1], papers[pi].name);

        unsigned W = ((h.cupsWidth + 511) / 512) * 512;   /* vendor pads to 512 px */
        unsigned H = h.cupsHeight, stride = W / 8;
        line = realloc(line, h.cupsBytesPerLine);
        bmp = realloc(bmp, (size_t)stride * H);
        if (!line || !bmp) { fprintf(stderr, "ERROR: rastertolhpl: out of memory\n"); return 1; }
        memset(bmp, 0, (size_t)stride * H);

        for (unsigned y = 0; y < H; y++) {
            if (cupsRasterReadPixels(ras, line, h.cupsBytesPerLine) < h.cupsBytesPerLine) {
                fprintf(stderr, "ERROR: rastertolhpl: short raster read on page %d\n", page + 1);
                return 1;
            }
            const unsigned char *thr = lhpl_screen[y & 31];
            unsigned char *row = bmp + (size_t)y * stride;
            for (unsigned x = 0; x < h.cupsWidth; x++) {
                unsigned g = invert ? 255 - line[x] : line[x];
                if (g < thr[x & 31]) row[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
            }
        }

        /* JBIG1 encode exactly like the vendor: 1 plane, L0=128, LRLTWO, SDRST. */
        jb.size = 0;
        struct jbg_enc_state enc; unsigned char *planes[1] = { bmp };
        jbg_enc_init(&enc, W, H, 1, planes, buf_cb, &jb);
        jbg_enc_options(&enc, 0, JBG_LRLTWO | JBG_SDRST, 128, 0, 0);
        jbg_enc_out(&enc);
        jbg_enc_free(&enc);

        /* Page header, JBIG payload and page end, collected per page. */
        pages = realloc(pages, (size_t)(page + 1) * sizeof *pages);
        if (!pages) { fprintf(stderr, "ERROR: rastertolhpl: out of memory\n"); return 1; }
        Buf *pg = &pages[page];
        pg->data = NULL; pg->size = 0; pg->cap = 0;
        unsigned char sp[57]; memset(sp, 0, sizeof sp);
        sp[0] = code; sp[1] = 0x01;
        put_u32(sp + 2, W);
        put_u32(sp + 6, H);
        put_u32(sp + 10, stride * H);
        put_u32(sp + 14, (unsigned)jb.size);
        put_u32(sp + 18, (unsigned)jb.size);
        put_u16(sp + 36, h.HWResolution[0] ? h.HWResolution[0] : 600);
        put_u16(sp + 38, (unsigned)(h.PageSize[0] * 254.0 / 72.0));   /* 0.1 mm, truncated */
        put_u16(sp + 40, (unsigned)(h.PageSize[1] * 254.0 / 72.0));
        write_block(pg, "sp", sp);
        buf_cb(jb.data, jb.size, pg);

        unsigned char ep[57]; memset(ep, 0, sizeof ep);
        write_block(pg, "ep", ep);

        page++;
        fprintf(stderr, "PAGE: %d %u\n", page, copies);
        fprintf(stderr, "INFO: rastertolhpl: page %d %s %ux%u -> %zu bytes JBIG\n",
                page, papers[pi].name, W, H, jb.size);
    }

    if (page == 0) { fprintf(stderr, "ERROR: rastertolhpl: no pages in raster stream\n"); return 1; }

    /* The vendor filter sends the last page first; do the same by default. */
    for (int i = 0; i < page; i++) {
        Buf *pg = &pages[reverse ? page - 1 - i : i];
        fwrite(pg->data, 1, pg->size, stdout);
        free(pg->data);
    }
    free(pages);

    printf("\x1b%%-12345X@PJL EOJ\r\n");
    fflush(stdout);
    cupsRasterClose(ras);
    if (fd) close(fd);
    free(line); free(bmp); free(jb.data);
    return 0;
}
