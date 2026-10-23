#include "v3.h"
void lv_receive(lv_rx *rx, const uint8_t *data, size_t length, int64_t now, lv_frame_fn frame,
                void *context) {
    if (rx->used && now - rx->last > 3000) {
        rx->used = 0;
        rx->dropping = true;
    }
    rx->last = now;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == '\n') {
            rx->text[rx->used] = 0;
            if (rx->used || rx->dropping)
                frame(context, rx->dropping ? "!" : rx->text, rx->dropping ? 1 : rx->used);
            rx->used = 0;
            rx->dropping = false;
        } else if (!rx->dropping) {
            if (rx->used == LV_REQUEST_MAX || data[i] == 0)
                rx->dropping = true;
            else
                rx->text[rx->used++] = (char)data[i];
        }
    }
}
