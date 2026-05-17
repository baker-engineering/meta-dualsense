#ifndef FFS_DESCRIPTORS_H
#define FFS_DESCRIPTORS_H
/* USB descriptors for the FunctionFS-backed DualSense gadget. Written to
 * /dev/ffs0/ep0 at startup, before any endpoints become usable. */

#include <endian.h>
#include <stdint.h>
#include <linux/usb/functionfs.h>
#include <linux/usb/ch9.h>

/* Constant-expression LE encoders. ARM is little-endian so these are no-ops,
 * but a typed cast keeps the field-type checks honest. glibc's htole16/32 are
 * not constant expressions, so we can't use them in static initializers. */
#define cpu_to_le16_const(x) ((uint16_t)(x))
#define cpu_to_le32_const(x) ((uint32_t)(x))

#define HID_DT_HID    0x21
#define HID_DT_REPORT 0x22

#define EP_IN_ADDR    0x81  /* IN  endpoint 1 */
#define EP_OUT_ADDR   0x02  /* OUT endpoint 2 */
#define EP_MAX_PACKET 64
#define HID_REPORT_DESC_SIZE 273   /* size of dualsense_report_descriptor[] */

/* HID class descriptor — class-specific, not in <linux/usb/ch9.h>. */
struct hid_class_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;   /* 0x21 */
    uint16_t bcdHID;            /* 0x0111 */
    uint8_t  bCountryCode;      /* 0 */
    uint8_t  bNumDescriptors;   /* 1 */
    uint8_t  bChildDescriptorType;  /* 0x22 Report */
    uint16_t wChildDescriptorLength;
} __attribute__((packed));

struct ffs_descriptors {
    struct usb_functionfs_descs_head_v2 header;
    uint32_t fs_count;
    uint32_t hs_count;
    /* FS speed */
    struct {
        struct usb_interface_descriptor    intf;
        struct hid_class_descriptor        hid;
        struct usb_endpoint_descriptor_no_audio ep_in;
        struct usb_endpoint_descriptor_no_audio ep_out;
    } __attribute__((packed)) fs;
    /* HS speed */
    struct {
        struct usb_interface_descriptor    intf;
        struct hid_class_descriptor        hid;
        struct usb_endpoint_descriptor_no_audio ep_in;
        struct usb_endpoint_descriptor_no_audio ep_out;
    } __attribute__((packed)) hs;
} __attribute__((packed));

static const struct ffs_descriptors descriptors = {
    .header = {
        .magic  = cpu_to_le32_const(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
        .length = cpu_to_le32_const(sizeof(struct ffs_descriptors)),
        .flags  = cpu_to_le32_const(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
    },
    .fs_count = cpu_to_le32_const(4),
    .hs_count = cpu_to_le32_const(4),
    .fs = {
        .intf = {
            .bLength            = sizeof(struct usb_interface_descriptor),
            .bDescriptorType    = USB_DT_INTERFACE,
            .bInterfaceNumber   = 0,
            .bAlternateSetting  = 0,
            .bNumEndpoints      = 2,
            .bInterfaceClass    = USB_CLASS_HID,
            .bInterfaceSubClass = 0,
            .bInterfaceProtocol = 0,
            .iInterface         = 0,
        },
        .hid = {
            .bLength               = sizeof(struct hid_class_descriptor),
            .bDescriptorType       = HID_DT_HID,
            .bcdHID                = cpu_to_le16_const(0x0111),
            .bCountryCode          = 0,
            .bNumDescriptors       = 1,
            .bChildDescriptorType  = HID_DT_REPORT,
            .wChildDescriptorLength = cpu_to_le16_const(HID_REPORT_DESC_SIZE),
        },
        .ep_in = {
            .bLength          = USB_DT_ENDPOINT_SIZE,
            .bDescriptorType  = USB_DT_ENDPOINT,
            .bEndpointAddress = EP_IN_ADDR,
            .bmAttributes     = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize   = cpu_to_le16_const(EP_MAX_PACKET),
            .bInterval        = 1,   /* FS: 1 ms */
        },
        .ep_out = {
            .bLength          = USB_DT_ENDPOINT_SIZE,
            .bDescriptorType  = USB_DT_ENDPOINT,
            .bEndpointAddress = EP_OUT_ADDR,
            .bmAttributes     = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize   = cpu_to_le16_const(EP_MAX_PACKET),
            .bInterval        = 1,
        },
    },
    .hs = {
        .intf = {
            .bLength            = sizeof(struct usb_interface_descriptor),
            .bDescriptorType    = USB_DT_INTERFACE,
            .bInterfaceNumber   = 0,
            .bAlternateSetting  = 0,
            .bNumEndpoints      = 2,
            .bInterfaceClass    = USB_CLASS_HID,
            .bInterfaceSubClass = 0,
            .bInterfaceProtocol = 0,
            .iInterface         = 0,
        },
        .hid = {
            .bLength               = sizeof(struct hid_class_descriptor),
            .bDescriptorType       = HID_DT_HID,
            .bcdHID                = cpu_to_le16_const(0x0111),
            .bCountryCode          = 0,
            .bNumDescriptors       = 1,
            .bChildDescriptorType  = HID_DT_REPORT,
            .wChildDescriptorLength = cpu_to_le16_const(HID_REPORT_DESC_SIZE),
        },
        .ep_in = {
            .bLength          = USB_DT_ENDPOINT_SIZE,
            .bDescriptorType  = USB_DT_ENDPOINT,
            .bEndpointAddress = EP_IN_ADDR,
            .bmAttributes     = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize   = cpu_to_le16_const(EP_MAX_PACKET),
            .bInterval        = 4,   /* HS: 2^(4-1)=8 microframes = 1 ms */
        },
        .ep_out = {
            .bLength          = USB_DT_ENDPOINT_SIZE,
            .bDescriptorType  = USB_DT_ENDPOINT,
            .bEndpointAddress = EP_OUT_ADDR,
            .bmAttributes     = USB_ENDPOINT_XFER_INT,
            .wMaxPacketSize   = cpu_to_le16_const(EP_MAX_PACKET),
            .bInterval        = 4,
        },
    },
};

/* Empty strings descriptor — we don't expose iInterface strings. */
static const struct {
    struct usb_functionfs_strings_head header;
} __attribute__((packed)) strings_descriptor = {
    .header = {
        .magic      = cpu_to_le32_const(FUNCTIONFS_STRINGS_MAGIC),
        .length     = cpu_to_le32_const(sizeof(struct usb_functionfs_strings_head)),
        .str_count  = cpu_to_le32_const(0),
        .lang_count = cpu_to_le32_const(0),
    },
};

/* 273-byte DualSense USB HID report descriptor, public reverse-engineered
 * value from github.com/nondebug/dualsense. */
static const uint8_t dualsense_report_descriptor[HID_REPORT_DESC_SIZE] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x21, 0x95, 0x0D, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26,
    0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02, 0x85, 0x02, 0x09, 0x23, 0x95, 0x2F, 0x91, 0x02,
    0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xB1, 0x02, 0x85, 0x08, 0x09, 0x34, 0x95, 0x2F, 0xB1, 0x02,
    0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xB1, 0x02, 0x85, 0x0A, 0x09, 0x25, 0x95, 0x1A, 0xB1, 0x02,
    0x85, 0x20, 0x09, 0x26, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x21, 0x09, 0x27, 0x95, 0x04, 0xB1, 0x02,
    0x85, 0x22, 0x09, 0x40, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x80, 0x09, 0x28, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x81, 0x09, 0x29, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x82, 0x09, 0x2A, 0x95, 0x09, 0xB1, 0x02,
    0x85, 0x83, 0x09, 0x2B, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x84, 0x09, 0x2C, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x85, 0x09, 0x2D, 0x95, 0x02, 0xB1, 0x02, 0x85, 0xA0, 0x09, 0x2E, 0x95, 0x01, 0xB1, 0x02,
    0x85, 0xE0, 0x09, 0x2F, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF0, 0x09, 0x30, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF1, 0x09, 0x31, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF2, 0x09, 0x32, 0x95, 0x0F, 0xB1, 0x02,
    0x85, 0xF4, 0x09, 0x35, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF5, 0x09, 0x36, 0x95, 0x03, 0xB1, 0x02,
    0xC0,
};

#endif
