#include <qrcodegen/qrcodegen.h>
#include <string.h>

#include "skiff/ui.h"

#define BITS_PER_BYTE 8
/* The generator's working buffers for the largest code (its own sizing rule). */
#define QR_BUFFER_BYTES qrcodegen_BUFFER_LEN_FOR_VERSION(SKIFF_UI_QR_VERSION_MAX)

_Static_assert(qrcodegen_VERSION_MAX >= SKIFF_UI_QR_VERSION_MAX,
               "the generator makes codes up to SKIFF_UI_QR_VERSION_MAX");

skiff_err skiff_ui_qr_encode(const char *text, skiff_ui_qr *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (text == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    uint8_t scratch[QR_BUFFER_BYTES];
    uint8_t code[QR_BUFFER_BYTES];
    if (strlen(text) > sizeof scratch ||
        !qrcodegen_encodeText(text, scratch, code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              SKIFF_UI_QR_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    const int size = qrcodegen_getSize(code);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(code, x, y)) {
                const int bit = y * size + x;
                out->modules[bit / BITS_PER_BYTE] |= (uint8_t)(1U << (bit % BITS_PER_BYTE));
            }
        }
    }
    out->size = size;
    return SKIFF_OK;
}

int skiff_ui_qr_dark(const skiff_ui_qr *qr, int x, int y) {
    if (qr == NULL || x < 0 || y < 0 || x >= qr->size || y >= qr->size) {
        return 0;
    }
    const int bit = y * qr->size + x;
    return (qr->modules[bit / BITS_PER_BYTE] >> (bit % BITS_PER_BYTE)) & 1;
}

int skiff_ui_qr_scale(const skiff_ui_qr *qr, int max_pixels) {
    if (qr == NULL || qr->size <= 0 || max_pixels <= 0) {
        return 0;
    }
    return max_pixels / (qr->size + 2 * SKIFF_UI_QR_QUIET_ZONE);
}
