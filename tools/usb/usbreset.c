// Port-resets the Elgato Video Capture (same effect as unplugging and replugging it).
// Use when a failed experiment leaves the device not answering control requests.
#include <libusb.h>
#include <stdio.h>

int main(void) {
    libusb_context *ctx;
    if (libusb_init(&ctx)) return 1;
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x0fd9, 0x0037);
    if (!h) { fprintf(stderr, "device not found\n"); return 1; }
    int r = libusb_reset_device(h);
    printf("reset: %s\n", r ? libusb_error_name(r) : "ok");
    if (r != LIBUSB_ERROR_NOT_FOUND) libusb_close(h);
    libusb_exit(ctx);
    return r ? 1 : 0;
}
