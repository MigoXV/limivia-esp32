#include "tx.h"
bool lv_tx_frame(lv_tx_io io, uint32_t generation, size_t payload, const char *text, size_t size) {
    if (!payload || !io.current(io.context, generation))
        return false;
    int64_t deadline = io.now(io.context) + 3000;
    size_t offset = 0;
    while (offset < size && io.current(io.context, generation) && io.now(io.context) < deadline) {
        size_t n = size - offset < payload ? size - offset : payload;
        int rc = io.fragment(io.context, generation, text + offset, n, deadline);
        if (rc < 0)
            break;
        if (rc > 0)
            offset += n;
        // Yield only on controller backpressure. Successful fragments are already
        // handed to the NimBLE host; a fixed delay per fragment throttles pages.
        if (!rc)
            io.pause(io.context);
    }
    if (offset == size)
        return true;
    // A truncated JSON line must never be followed by a different line on the
    // same BLE connection. Check epoch so this cannot disconnect a replacement.
    if (io.current(io.context, generation))
        io.abort(io.context, generation);
    return false;
}
