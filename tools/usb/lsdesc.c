// Dumps the full USB descriptor tree of the Elgato Video Capture (0fd9:0037),
// including vendor-specific (class-specific) descriptor bytes. Read-only.
// build: cc lsdesc.c -o lsdesc $(pkg-config --cflags --libs libusb-1.0)
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>

static void hexdump(const char *label, const unsigned char *p, int n) {
    if (n <= 0) return;
    printf("%s (%d bytes):", label, n);
    for (int i = 0; i < n; i++) printf("%s%02x", (i % 16) ? " " : "\n      ", p[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    unsigned vid = argc > 1 ? strtoul(argv[1], 0, 16) : 0x0fd9;
    unsigned pid = argc > 2 ? strtoul(argv[2], 0, 16) : 0x0037;
    libusb_context *ctx;
    if (libusb_init(&ctx)) return 1;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        libusb_get_device_descriptor(list[i], &dd);
        if (dd.idVendor != vid || dd.idProduct != pid) continue;
        printf("device %04x:%04x bcdDevice=%04x bcdUSB=%04x class=%02x/%02x/%02x ep0=%d configs=%d speed=%d\n",
               dd.idVendor, dd.idProduct, dd.bcdDevice, dd.bcdUSB, dd.bDeviceClass, dd.bDeviceSubClass,
               dd.bDeviceProtocol, dd.bMaxPacketSize0, dd.bNumConfigurations, libusb_get_device_speed(list[i]));
        for (int c = 0; c < dd.bNumConfigurations; c++) {
            struct libusb_config_descriptor *cd;
            if (libusb_get_config_descriptor(list[i], c, &cd)) continue;
            printf("config %d: wTotalLength=%d interfaces=%d attrs=%02x maxpower=%dmA\n", cd->bConfigurationValue,
                   cd->wTotalLength, cd->bNumInterfaces, cd->bmAttributes, cd->MaxPower * 2);
            hexdump("  config extra", cd->extra, cd->extra_length);
            for (int f = 0; f < cd->bNumInterfaces; f++) {
                const struct libusb_interface *itf = &cd->interface[f];
                for (int a = 0; a < itf->num_altsetting; a++) {
                    const struct libusb_interface_descriptor *id = &itf->altsetting[a];
                    printf("  if %d alt %d: class=%02x/%02x/%02x eps=%d iInterface=%d\n", id->bInterfaceNumber,
                           id->bAlternateSetting, id->bInterfaceClass, id->bInterfaceSubClass,
                           id->bInterfaceProtocol, id->bNumEndpoints, id->iInterface);
                    hexdump("    if extra", id->extra, id->extra_length);
                    for (int e = 0; e < id->bNumEndpoints; e++) {
                        const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
                        static const char *tt[] = {"ctrl", "isoc", "bulk", "intr"};
                        int mps = ed->wMaxPacketSize & 0x7ff, mult = ((ed->wMaxPacketSize >> 11) & 3) + 1;
                        printf("    ep %02x %s %s wMaxPacketSize=0x%04x (%d x %d = %d) interval=%d\n",
                               ed->bEndpointAddress, (ed->bEndpointAddress & 0x80) ? "IN " : "OUT",
                               tt[ed->bmAttributes & 3], ed->wMaxPacketSize, mps, mult, mps * mult, ed->bInterval);
                        hexdump("      ep extra", ed->extra, ed->extra_length);
                    }
                }
            }
            libusb_free_config_descriptor(cd);
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}
