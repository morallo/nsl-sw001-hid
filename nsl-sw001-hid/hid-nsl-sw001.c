// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hid.h>
#include <linux/module.h>

#define USB_VENDOR_ID_NINTENDO 0x057e
#define USB_DEVICE_ID_PRO_CONTROLLER 0x2009

/*
 * N-SL SW001 descriptor, rewritten to match the controller's real
 * Bluetooth HID input report (0x30).
 *
 * The controller identifies as a Pro Controller (057e:2009) but does NOT
 * use the Nintendo Pro Controller HID protocol, and its real 0x30 report
 * (49 bytes = report id + 48 data bytes) has a completely different
 * layout than what the vendor descriptor declares.  Layout was derived
 * by decoding Bluetooth captures while pressing each control one at a
 * time (see capdecode.py).  Wire format: <a1><0x30><48 bytes>.
 *
 * Actually-observed fields:
 *
 *   byte 0  sequence counter (0..255, +1 per report)
 *   byte 1  0x80 at rest (flags/type)
 *   byte 2  buttons: bit0=Y bit1=X bit2=B bit3=A bit6=R bit7=ZR
 *   byte 3  buttons: bit0=Minus bit1=Plus bit2=SR bit3=SL bit4=Home
 *                    bit5=Capture
 *   byte 4  D-pad:   bit0=Dwn bit1=Up bit2=Right bit3=Left
 *                    bit6=L    bit7=ZL
 *   byte 5  left stick X  low byte          \
 *   byte 6  left stick X/Y middle (12-bit)  | JoyCon-style 12-bit packing
 *   byte 7  left stick Y  high byte         /  center ~0x800, range 0-4095
 *   byte 8  right stick X low byte
 *   byte 9  right stick X/Y middle
 *   byte 10 right stick Y high byte
 *   byte 11 0x0a (constant)
 *   bytes 12..47  three 12-byte IMU samples (ignored here)
 *
 * The 12-bit stick packing means byte 6/9 share 4 bits between the two
 * axes, which the 4 x 12-bit field below maps natively.
 */
static const __u8 nsl_sw001_rdesc[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x05,       /* Usage (Game Pad) */
    0xa1, 0x01,       /* Collection (Application) */

    0x85, 0x30,       /* Report ID (0x30) */

    /* byte 0: sequence counter, ignore */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x01,       /* Input (Const) */

    /* byte 1: flags/type, ignore */
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x01,

    /*
     * byte 2: buttons
     *   bit0=Y bit1=X bit2=B bit3=A bit6=R bit7=ZR
     *   bits 4-5 unused (constant)
     *
     * NOTE: under a Game Pad application collection, hid-input maps
     * Button-page usage N to code ((N-1) & 0xff) + BTN_GAMEPAD, i.e.
     * 0x130 + (N-1).  Usage values below are chosen accordingly:
     *   Y=0x05->BTN_WEST      X=0x04->BTN_NORTH
     *   B=0x02->BTN_EAST      A=0x01->BTN_SOUTH
     *   R=0x08->BTN_TR        ZR=0x0a->BTN_TR2
     */
    0x05, 0x09,       /* Usage Page (Button) */
    0x09, 0x05,       /* Usage (Y) */
    0x09, 0x04,       /* Usage (X) */
    0x09, 0x02,       /* Usage (B) */
    0x09, 0x01,       /* Usage (A) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x04,       /* Report Count (4) */
    0x81, 0x02,       /* Input (Data,Var,Abs) */
    0x75, 0x01,
    0x95, 0x02,
    0x81, 0x01,       /* Input (Const) */
    0x09, 0x08,       /* Usage (R) */
    0x09, 0x0a,       /* Usage (ZR) */
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x02,
    0x81, 0x02,

    /*
     * byte 3: buttons
     *   bit0=Minus bit1=Plus bit2=SR bit3=SL bit4=Home bit5=Capture
     *   bits 6-7 unused (constant)
     *
     * usage -> keycode (gamepad base 0x130 + (N-1)):
     *   Minus=0x0b->BTN_SELECT  Plus=0x0c->BTN_START
     *   SR=0x0f->BTN_THUMBR     SL=0x0e->BTN_THUMBL
     *   Home=0x0d->BTN_MODE     Capture=0x06->BTN_Z
     */
    0x09, 0x0b,       /* Usage (Minus) */
    0x09, 0x0c,       /* Usage (Plus) */
    0x09, 0x0f,       /* Usage (SR) */
    0x09, 0x0e,       /* Usage (SL) */
    0x09, 0x0d,       /* Usage (Home) */
    0x09, 0x06,       /* Usage (Capture) */
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x06,
    0x81, 0x02,
    0x75, 0x01,
    0x95, 0x02,
    0x81, 0x01,

    /*
     * byte 4: D-pad + L/ZL
     *   bit0=Dwn bit1=Up bit2=Right bit3=Left
     *   bits 4-5 unused (constant)
     *   bit6=L -> BTN_TL   bit7=ZL -> BTN_TL2
     *
     * D-pad directions are declared as Generic Desktop usages
     * (0x91/0x90/0x92/0x93) but BLOCKED from the broken generic
     * 1-bit hat path by input_mapping, which remaps each 1-bit
     * direction to BTN_DPAD_*.  See nsl_sw001_input_mapping().
     *
     * L/ZL usages (gamepad base):  L=0x07->BTN_TL, ZL=0x09->BTN_TL2
     */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x91,       /* Usage (D-pad Down) */
    0x09, 0x90,       /* Usage (D-pad Up) */
    0x09, 0x92,       /* Usage (D-pad Right) */
    0x09, 0x93,       /* Usage (D-pad Left) */
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x04,
    0x81, 0x02,
    0x75, 0x01,
    0x95, 0x02,
    0x81, 0x01,
    0x05, 0x09,       /* Usage Page (Button) */
    0x09, 0x07,       /* Usage (L) */
    0x09, 0x09,       /* Usage (ZL) */
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x02,
    0x81, 0x02,

    /*
     * bytes 5..10: analog sticks, 4x 12-bit.
     *   X -> left stick X, Y -> left stick Y
     *   Rx -> right stick X, Ry -> right stick Y
     * Rest position ~0x800, full range 0..4095.
     */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x30,       /* Usage (X) */
    0x09, 0x31,       /* Usage (Y) */
    0x09, 0x33,       /* Usage (Rx) */
    0x09, 0x34,       /* Usage (Ry) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x26, 0xff, 0x0f, /* Logical Maximum (4095) */
    0x75, 0x0c,       /* Report Size (12) */
    0x95, 0x04,       /* Report Count (4) */
    0x81, 0x02,       /* Input (Data,Var,Abs) */

    /*
     * bytes 11..47: 0x0a status byte + three 12-byte IMU samples
     * (each sample = accel X/Y/Z, gyro X/Y/Z as int16 LE).
     *
     * Byte 11 (0x0a) and bytes 12..35 (samples 1-2) are ignored.
     * Bytes 36..47 (newest sample) are declared as Sensor usages and
     * mapped to EV_ABS axes by nsl_sw001_input_mapping():
     *   absmisc+0=accU   absmisc+1=accY  absmisc+2=accZ
     *   absmisc+3=gyroX  absmisc+4=gyroY absmisc+5=gyroZ
     */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x01,       /* Input (Const)      -- byte 11 status */
    0x75, 0x08,
    0x95, 0x18,       /* Report Count (24)  -- bytes 12..35 */
    0x81, 0x01,       /* Input (Const) */
    0x05, 0x20,       /* Usage Page (Sensor) */
    0x09, 0x73,       /* Usage (Accelerometer 3D) */
    0x09, 0x73,       /* Usage (Accelerometer 3D) */
    0x09, 0x73,       /* Usage (Accelerometer 3D) */
    0x09, 0x76,       /* Usage (Gyrometer 3D) */
    0x09, 0x76,       /* Usage (Gyrometer 3D) */
    0x09, 0x76,       /* Usage (Gyrometer 3D) */
    0x16, 0x00, 0x80, /* Logical Minimum (-32768) */
    0x26, 0xff, 0x7f, /* Logical Maximum (32767) */
    0x75, 0x10,       /* Report Size (16) */
    0x95, 0x06,       /* Report Count (6) */
    0x81, 0x02,       /* Input (Data,Var,Abs)  -- bytes 36..47 */

    0xc0,

    /*
     * Original mouse collection (report id 0x02), unchanged.
     */
    0x05, 0x01,
    0x09, 0x02,
    0xa1, 0x01,
    0x85, 0x02,

    0x09, 0x01,
    0xa1, 0x00,

    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x03,
    0x81, 0x02,

    0x75, 0x01,
    0x95, 0x05,
    0x81, 0x03,

    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7f,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,

    0xc0,
    0xc0,

    /*
     * Original consumer-control collection, unchanged.
     */
    0x05, 0x0c,
    0x09, 0x01,
    0xa1, 0x01,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,

    0x0a, 0x83, 0x01,
    0x0a, 0x23, 0x02,
    0x0a, 0x24, 0x02,
    0x09, 0x40,
    0x09, 0xe9,
    0x09, 0xea,

    0x05, 0x07,
    0x09, 0x28,
    0x09, 0x75,

    0x81, 0x02,
    0xc0
};

/*
 * The controller reports the D-pad as four independent 1-bit values
 * (Down/Up/Right/Left).  hid-input's generic Generic-Desktop D-pad
 * handling only maps the first direction found in a field to a hat
 * axis and discards the rest, which is broken for this layout.  Map
 * each 1-bit direction to its own keycode (BTN_DPAD_*) instead.
 */
static int nsl_sw001_input_mapping(struct hid_device *hdev,
                                   struct hid_input *hidinput,
                                   struct hid_field *field,
                                   struct hid_usage *usage,
                                   unsigned long **bit, int *max)
{
    if (field->application != HID_GD_GAMEPAD)
        return 0;

    /* Newest IMU sample (bytes 36..47): accel X/Y/Z then gyro X/Y/Z,
     * 6x int16 declared as Sensor usages.  Expose them as extra EV_ABS
     * axes (ABS_MISC + 0..5) so they are visible in evdev tools and can
     * feed a userspace DSU / Cemuhook bridge (Cemu, Yuzu, Ryujinx).
     */
    if ((usage->hid & HID_USAGE_PAGE) == HID_UP_SENSOR) {
        int idx = usage->usage_index;
        if (idx < 6)
            hid_map_usage(hidinput, usage, bit, max, EV_ABS, ABS_MISC + idx);
        return 1;
    }

    switch (usage->hid) {
    case HID_GD_UP:
        hid_map_usage(hidinput, usage, bit, max, EV_KEY, BTN_DPAD_UP);
        return 1;
    case HID_GD_DOWN:
        hid_map_usage(hidinput, usage, bit, max, EV_KEY, BTN_DPAD_DOWN);
        return 1;
    case HID_GD_RIGHT:
        hid_map_usage(hidinput, usage, bit, max, EV_KEY, BTN_DPAD_RIGHT);
        return 1;
    case HID_GD_LEFT:
        hid_map_usage(hidinput, usage, bit, max, EV_KEY, BTN_DPAD_LEFT);
        return 1;
    default:
        return 0;
    }
}

/*
 * hid-core's generic mapping gives EV_ABS axes in GAMEPAD applications a
 * fuzz/flat derived from the logical range.  For the IMU axes (logical
 * -32768..32767) that means fuzz=255 and flat=4095: input core suppresses
 * any sample within ~4095 counts of center, i.e. all real gyro/accel data.
 * Clear it so raw 16-bit samples reach userspace.
 */
static int nsl_sw001_input_configured(struct hid_device *hdev,
                                       struct hid_input *hidinput)
{
    struct input_dev *input = hidinput->input;
    int i;

    for (i = 0; i < 6; i++) {
        if (test_bit(ABS_MISC + i, input->absbit))
            input_set_abs_params(input, ABS_MISC + i, -32768, 32767, 0, 0);
    }
    return 0;
}

static const __u8 *nsl_sw001_report_fixup(struct hid_device *hdev,
                                          __u8 *rdesc,
                                          unsigned int *rsize)
{
    if (hdev->vendor != USB_VENDOR_ID_NINTENDO ||
        hdev->product != USB_DEVICE_ID_PRO_CONTROLLER)
        return rdesc;

    hid_info(hdev, "applying N-SL SW001 HID report descriptor fix\n");

    *rsize = sizeof(nsl_sw001_rdesc);
    return nsl_sw001_rdesc;
}

static int nsl_sw001_probe(struct hid_device *hdev,
                           const struct hid_device_id *id)
{
    int ret;

    hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

    ret = hid_parse(hdev);
    if (ret) {
        hid_err(hdev, "HID parse failed: %d\n", ret);
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) {
        hid_err(hdev, "HID hardware start failed: %d\n", ret);
        return ret;
    }

    return 0;
}

static const struct hid_device_id nsl_sw001_devices[] = {
    {
        HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO,
                             USB_DEVICE_ID_PRO_CONTROLLER)
    },
    { }
};

MODULE_DEVICE_TABLE(hid, nsl_sw001_devices);

static struct hid_driver nsl_sw001_driver = {
    .name = "hid-nsl-sw001",
    .id_table = nsl_sw001_devices,
    .probe = nsl_sw001_probe,
    .report_fixup = nsl_sw001_report_fixup,
    .input_mapping = nsl_sw001_input_mapping,
    .input_configured = nsl_sw001_input_configured,
};

module_hid_driver(nsl_sw001_driver);

MODULE_DESCRIPTION("HID quirk for N-SL SW001 Nintendo-style controller");
MODULE_AUTHOR("Test driver");
MODULE_LICENSE("GPL");
