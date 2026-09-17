/*
 * pdftoraster_cg - CUPS filter: PDF -> CUPS raster, using only Apple system
 * frameworks (CoreGraphics/ImageIO) plus libcups. No Ghostscript, so it runs
 * inside the CUPS filter sandbox on macOS 26/27. Replacement for Apple's removed
 * cgpdftoraster, tuned to the RICOH SP 150: 8-bit DeviceGray, cupsColorSpace W,
 * 600 dpi, all nine PPD paper sizes, copies passed on via NumCopies.
 *
 * Orientation: we render into a TOP-LEFT-origin bitmap (flipped context) so the
 * pixel buffer is already in CUPS row order (top first, left to right).
 *
 * Tone: cupsColorSpace W means 255 = white/no-toner, 0 = black/toner. That is
 * exactly DeviceGray, so we send the rendered gray as-is (no inversion) by
 * default. PDFTORICOH_INVERT=1 forces inversion (debug aid).
 *
 * Debug: PDFTORICOH_DUMP_PNG=/path writes page 1 as PNG for visual verification.
 *
 * CUPS invokes filters as:  filter job user title copies options [filename]
 *
 * Build: clang -O2 -o pdftoraster_cg pdftoraster_cg.c \
 *        -framework CoreGraphics -framework CoreFoundation -framework ImageIO \
 *        -lcupsimage -lcups
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <cups/raster.h>

static const double DPI = 600.0;

static CFDataRef read_stdin(void)
{
    CFMutableDataRef d = CFDataCreateMutable(NULL, 0);
    unsigned char buf[65536];
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0)
        CFDataAppendBytes(d, buf, n);
    return d;
}

/* Write an 8-bit gray, top-first buffer as a PNG (for debugging orientation/tone). */
static void dump_png(const char *path, const unsigned char *buf, size_t w, size_t h)
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef c = CGBitmapContextCreate((void *)buf, w, h, 8, w, cs, kCGImageAlphaNone);
    CGImageRef img = CGBitmapContextCreateImage(c);
    CFStringRef p = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef u = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(u, CFSTR("public.png"), 1, NULL);
    if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
    CFRelease(u); CFRelease(p); CGImageRelease(img);
    CGContextRelease(c); CGColorSpaceRelease(cs);
}

int main(int argc, char **argv)
{
    int invert = 0;
    const char *ei = getenv("PDFTORICOH_INVERT");
    if (ei && ei[0] == '1') invert = 1;
    const char *png = getenv("PDFTORICOH_DUMP_PNG");

    /* Paper sizes the SP 150 PPD offers, in points. Default A4. */
    static const struct { const char *name; double w, h; } papers[] = {
        { "A4", 595, 842 }, { "Letter", 612, 792 }, { "A5", 420, 595 }, { "A6", 297, 420 },
        { "B5", 516, 729 }, { "B6", 363, 516 }, { "Executive", 522, 756 },
        { "16K", 524, 737 }, { "Legal", 612, 1008 },
    };
    double mediaW = 595.0, mediaH = 842.0;
    const char *pname = "A4";
    const char *opts = (argc > 5) ? argv[5] : "";
    const char *keys[] = { "PageSize=", "media=" };
    for (int k = 0; k < 2 && pname == papers[0].name; k++) {
        const char *v = strcasestr(opts, keys[k]);
        if (!v) continue;
        v += strlen(keys[k]);
        for (size_t i = 0; i < sizeof papers / sizeof papers[0]; i++) {
            size_t n = strlen(papers[i].name);
            if (strncasecmp(v, papers[i].name, n) == 0 && (v[n] == 0 || v[n] == ' ' || v[n] == ',')) {
                mediaW = papers[i].w; mediaH = papers[i].h; pname = papers[i].name; break;
            }
        }
    }
    int copies = (argc > 4) ? atoi(argv[4]) : 1;
    if (copies < 1) copies = 1;

    CGPDFDocumentRef doc = NULL;
    const char *infile = (argc > 6 && argv[6] && argv[6][0]) ? argv[6] : NULL;
    if (infile) {
        CFStringRef p = CFStringCreateWithCString(NULL, infile, kCFStringEncodingUTF8);
        CFURLRef u = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, false);
        doc = CGPDFDocumentCreateWithURL(u);
        CFRelease(u); CFRelease(p);
    } else {
        CFDataRef data = read_stdin();
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
        doc = CGPDFDocumentCreateWithProvider(prov);
        CGDataProviderRelease(prov); CFRelease(data);
    }
    if (!doc) { fprintf(stderr, "ERROR: pdftoraster_cg: cannot open PDF\n"); return 1; }

    size_t npages = CGPDFDocumentGetNumberOfPages(doc);
    if (npages == 0) { fprintf(stderr, "ERROR: pdftoraster_cg: PDF has 0 pages\n"); return 1; }

    size_t pxW = (size_t)(mediaW / 72.0 * DPI + 0.5);
    size_t pxH = (size_t)(mediaH / 72.0 * DPI + 0.5);
    size_t bpr = pxW;

    cups_raster_t *ras = cupsRasterOpen(1, CUPS_RASTER_WRITE);
    if (!ras) { fprintf(stderr, "ERROR: pdftoraster_cg: cupsRasterOpen failed\n"); return 1; }

    unsigned char *bmp = malloc(bpr * pxH);
    unsigned char *line = malloc(bpr);
    if (!bmp || !line) { fprintf(stderr, "ERROR: pdftoraster_cg: OOM\n"); return 1; }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();

    for (size_t pg = 1; pg <= npages; pg++) {
        CGPDFPageRef page = CGPDFDocumentGetPage(doc, pg);
        if (!page) continue;

        memset(bmp, 0xFF, bpr * pxH);   /* white background */
        CGContextRef ctx = CGBitmapContextCreate(bmp, pxW, pxH, 8, bpr, cs, kCGImageAlphaNone);
        if (!ctx) { fprintf(stderr, "ERROR: pdftoraster_cg: bitmap ctx failed\n"); return 1; }
        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
        CGContextSetShouldAntialias(ctx, true);

        /* CG bitmap contexts already store buffer row 0 as the top scanline, so
         * no vertical flip is needed. Just scale to points at 600 dpi. */
        CGContextScaleCTM(ctx, DPI / 72.0, DPI / 72.0);
        CGRect media = CGRectMake(0, 0, mediaW, mediaH);
        CGAffineTransform t = CGPDFPageGetDrawingTransform(page, kCGPDFCropBox, media, 0, true);
        CGContextConcatCTM(ctx, t);
        CGContextClipToRect(ctx, CGPDFPageGetBoxRect(page, kCGPDFCropBox));
        CGContextDrawPDFPage(ctx, page);
        CGContextRelease(ctx);

        if (png && pg == 1) dump_png(png, bmp, pxW, pxH);

        cups_page_header2_t h;
        memset(&h, 0, sizeof(h));
        h.HWResolution[0] = h.HWResolution[1] = (unsigned)DPI;
        h.cupsWidth = (unsigned)pxW;
        h.cupsHeight = (unsigned)pxH;
        h.cupsBitsPerColor = 8;
        h.cupsBitsPerPixel = 8;
        h.cupsBytesPerLine = (unsigned)bpr;
        h.cupsColorOrder = CUPS_ORDER_CHUNKED;
        h.cupsColorSpace = CUPS_CSPACE_W;
        h.cupsNumColors = 1;
        h.NumCopies = (unsigned)copies;
        strncpy(h.cupsPageSizeName, pname, sizeof(h.cupsPageSizeName) - 1);
        h.PageSize[0] = (unsigned)(mediaW + 0.5);
        h.PageSize[1] = (unsigned)(mediaH + 0.5);
        h.cupsPageSize[0] = (float)mediaW;
        h.cupsPageSize[1] = (float)mediaH;
        h.ImagingBoundingBox[2] = h.PageSize[0];
        h.ImagingBoundingBox[3] = h.PageSize[1];
        h.cupsImagingBBox[2] = (float)mediaW;
        h.cupsImagingBBox[3] = (float)mediaH;

        if (!cupsRasterWriteHeader2(ras, &h)) {
            fprintf(stderr, "ERROR: pdftoraster_cg: WriteHeader failed\n"); return 1;
        }

        for (size_t y = 0; y < pxH; y++) {      /* buffer is already top-first */
            unsigned char *src = bmp + y * bpr;
            if (invert) { for (size_t x = 0; x < bpr; x++) line[x] = (unsigned char)(255 - src[x]); }
            else        memcpy(line, src, bpr);
            if (cupsRasterWritePixels(ras, line, (unsigned)bpr) < (unsigned)bpr) {
                fprintf(stderr, "ERROR: pdftoraster_cg: WritePixels failed\n"); return 1;
            }
        }
        fprintf(stderr, "INFO: pdftoraster_cg: page %zu/%zu (%zux%zu) invert=%d\n",
                pg, npages, pxW, pxH, invert);
    }

    cupsRasterClose(ras);
    CGColorSpaceRelease(cs);
    CGPDFDocumentRelease(doc);
    free(bmp); free(line);
    return 0;
}
