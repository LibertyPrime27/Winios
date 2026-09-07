/* Decoding the pictures a program carries.
 *
 * A Windows program keeps its artwork in its own resource directory as
 * device-independent bitmaps: the installer's banner and its wizard-side
 * image, the icons in its title bar and its list views, the cursors it sets.
 * None of that is a file format anybody chose recently -- it is the 1990
 * DIB, in every bit depth from 1 to 32, plus an icon container that stacks
 * several DIBs and a transparency mask underneath each one.
 *
 * Until this existed, LoadImage and friends returned a plausible handle and
 * drew nothing, so an installer came up with holes where its artwork should
 * be. Decoding is the whole fix: the blitters were already real.
 */
#ifndef W32_IMAGE_H
#define W32_IMAGE_H
#include <stdint.h>
#include <stddef.h>

/* Straight (not premultiplied) 0xAARRGGBB, top-down. `px` is malloc'd. */
typedef struct { int w, h; uint32_t *px; } w32_image;

/* A packed DIB: BITMAPINFOHEADER, palette, pixels -- what an RT_BITMAP
 * resource holds, and what a .bmp file holds after its 14-byte file header.
 * Returns 1 and fills `out` on success. */
int w32_dib_decode(const uint8_t *d, size_t n, w32_image *out);

/* One icon or cursor image: the same, except the header's height counts the
 * colour rows *and* the 1-bit AND mask that follows them, and the mask
 * becomes alpha for the depths that have none of their own. */
int w32_icon_decode(const uint8_t *d, size_t n, w32_image *out);

/* A whole .ico / .cur file, or an RT_GROUP_ICON directory paired with a way
 * to fetch each RT_ICON by id. `want` is the preferred square size; 0 takes
 * the largest. */
typedef const uint8_t *(*w32_res_fetch)(void *ctx, int id, uint32_t *size);
int w32_icon_group(const uint8_t *dir, size_t n, int want,
                   w32_res_fetch fetch, void *ctx, w32_image *out, int *hx, int *hy);
int w32_ico_file(const uint8_t *d, size_t n, int want, w32_image *out, int *hx, int *hy);

/* The stock cursors, drawn here because they are not in anybody's resources:
 * IDC_ARROW and the rest come out of the system, and a program that sets one
 * still expects to see it. `id` is the IDC_* number. */
int w32_stock_cursor(int id, w32_image *out, int *hx, int *hy);
/* The stock icons behind IDI_APPLICATION and the message-box symbols. */
int w32_stock_icon(int id, int size, w32_image *out);

void w32_image_free(w32_image *im);

#endif
