// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/unaligned.h>

#define USB_VENDOR_ID_NINTENDO 0x057e
#define USB_DEVICE_ID_PRO_CONTROLLER 0x2009

/*
 * OUI (first three bytes of the Bluetooth BD_ADDR) that identifies the
 * N-SL SW001 clones.  It is an unregistered IEEE OUI block, shared by
 * the whole family of SW001 units; each unit has its own MAC suffix.
 * Genuine Nintendo controllers use Nintendo's registered OUIs, so keying
 * on this prefix lets the driver claim only SW001 units (both of ours,
 * and any other unit of the same model) without matching 057e:2009
 * devices of other vendors.
 */
#define NSL_SW001_OUI0 0x9c
#define NSL_SW001_OUI1 0x54
#define NSL_SW001_OUI2 0x00

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

/*
 * IMU axis ranges/resolutions, matching upstream hid-nintendo
 * (drivers/hid/hid-nintendo.c).  The reported values are raw/uncalibrated
 * 16-bit samples but are normalized to these advertised resolutions so
 * SDL's evdev sensor backend can scale them to physical units.
 */
#define NSL_IMU_MAX_ACCEL_MAG   32767
#define NSL_IMU_ACCEL_RES_PER_G 4096    /* counts per G */
#define NSL_IMU_MAX_GYRO_MAG     32767000 /* (2^16-1)*1000 */
#define NSL_IMU_GYRO_RES_PER_DPS 14247   /* (14.247*1000) counts per deg/s */
#define NSL_IMU_FUZZ             10

struct nsl_sw001_data {
    /* Separate IMU input device (accel ABS_X/Y/Z, gyro ABS_RX/RY/RZ) */
    struct input_dev *imu_dev;
    /* Gamepad input device (from hid-input), used for the D-pad hat */
    struct input_dev *gamepad_dev;
    /* Synthetic per-report timestamp counter (us) for MSC_TIMESTAMP */
    s64 imu_timestamp_us;
};

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
     * 0x130 + (N-1).  Usage values below are chosen accordingly; the
     * physical A (right face) resolves to BTN_EAST and the physical B
     * (bottom face) to BTN_SOUTH, the physically-correct codes:
     *   Y=0x05->BTN_WEST      X=0x04->BTN_NORTH
     *   B=0x02->BTN_SOUTH     A=0x01->BTN_EAST
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
     * The four D-pad direction bits are declared CONST so hid-input does
     * not turn them into separate BTN_DPAD_* buttons (which hid-nintendo
     * does not expose and Steam cannot read on its Switch Pro profile).
     * Instead the axes ABX_HAT0X/HAT0Y are registered on the gamepad node
     * (see input_configured) and emitted here from the raw direction bits,
     * matching how hid-nintendo reports a genuine Pro Controller's D-pad
     * as an 8-way hat switch that Steam maps to dpup/dpdown/dpleft/dpright.
     *
     * L/ZL usages (gamepad base):  L=0x07->BTN_TL, ZL=0x09->BTN_TL2
     */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x75, 0x04,
    0x95, 0x01,
    0x81, 0x01,       /* Input (Const) -- 4 D-pad bits (hat, handled below) */
    0x75, 0x01,
    0x95, 0x02,
    0x81, 0x01,       /* Input (Const) -- bits 4-5 */
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
     * Byte 11 (0x0a) and bytes 12..47 (all three IMU samples) are
     * ignored by HID core here: the newest sample (bytes 36..47) is
     * instead parsed in nsl_sw001_raw_event() and reported to the
     * separate IMU input device (accel ABS_X/Y/Z, gyro ABS_RX/RY/RZ),
     * matching how hid-nintendo / hid-sony expose it.
     */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x01,       /* Input (Const)      -- byte 11 status */
    0x75, 0x08,
    0x95, 0x24,       /* Report Count (36)  -- bytes 12..47 */
    0x81, 0x01,       /* Input (Const) */

    0xc0,

    /*
     * The original consumer-control and mouse collections are deliberately
     * omitted: the SW001 never sends those reports (it only emits report
     * ID 0x30), and leaving them in makes hid-input (with INPUT_PER_APP)
     * register phantom extra input devices and button/key bits.  The tool
     * is a pure gamepad, so only the Game Pad collection is kept.
     */
};

static int nsl_sw001_input_mapping(struct hid_device *hdev,
                                   struct hid_input *hidinput,
                                   struct hid_field *field,
                                   struct hid_usage *usage,
                                   unsigned long **bit, int *max)
{
    if (field->application != HID_GD_GAMEPAD)
        return 0;

    switch (usage->hid) {
    /*
     * Map physical A (right/east face, physical usage 0x01) to BTN_EAST
     * (index 1) and physical B (bottom/south face, physical usage 0x02) to
     * BTN_SOUTH (index 0), i.e. the physically-correct codes at their
     * physical positions (matching how a genuine Nintendo Switch Pro
     * Controller is laid out).  The controllerdb entry then reports the
     * matching a/b roles (a:b0,b:b1) so userspace sees the right mapping.
     */
    case 0x00090001:   /* Usage (A) -- Nintendo right face -> BTN_EAST */
        hid_map_usage(hidinput, usage, bit, max, EV_KEY, BTN_EAST);
        return 1;
    case 0x00090002:   /* Usage (B) -- Nintendo bottom face -> BTN_SOUTH */
        hid_map_usage(hidinput, usage, bit, max, EV_KEY, BTN_SOUTH);
        return 1;
    default:
        return 0;
    }
}

/*
 * The D-pad is emitted as an 8-way hat switch (ABS_HAT0X/HAT0Y), matching
 * how hid-nintendo reports a genuine Pro Controller so that Steam's built-in
 * Switch Pro profile (dpup/dpdown/dpleft/dpright on hat 0) works.  Register
 * the two hat axes on the gamepad node and remember that node for raw_event.
 */
static int nsl_sw001_input_configured(struct hid_device *hdev,
                                       struct hid_input *hidinput)
{
    struct nsl_sw001_data *drvdata = hid_get_drvdata(hdev);

    if (hidinput->application == HID_GD_GAMEPAD) {
        struct input_dev *input = hidinput->input;
        input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
        input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);
        drvdata->gamepad_dev = input;
    }
    return 0;
}

/*
 * The controller reports the two analog Y axes with a reversed direction
 * relative to the HID/evdev convention (pushing down drives the raw value
 * toward 0, which hid-input would surface as the negative/-up extreme).
 * Invert the two 12-bit Y fields here, before hid-input parses the field
 * values, so that userspace sees the standard convention (down=+, up=-)
 * with the rest still centered.  No SDL axis negation is then needed.
 *
 * The same hook also extracts the newest 12-byte IMU sample (bytes
 * data[37..48]: accel X/Y/Z then gyro X/Y/Z as int16 LE) and reports it to
 * the separate IMU input device created in probe, mirroring how
 * hid-nintendo / hid-sony expose it.
 *
 * Report layout (see header): data[0] is the report id (0x30), the sticks
 * are bit-packed 12-bit values over data[6..11]:
 *   X  = data[6] | ((data[7] & 0x0f) << 8)
 *   Y  = (data[7] >> 4) | (data[8] << 4)
 *   Rx = data[9] | ((data[10] & 0x0f) << 8)
 *   Ry = (data[10] >> 4) | (data[11] << 4)
 * and the newest IMU sample occupies data[37..48].
 */
static int nsl_sw001_raw_event(struct hid_device *hdev,
                               struct hid_report *report,
                               u8 *data, int size)
{
    unsigned int y, ry;
    struct nsl_sw001_data *drvdata = hid_get_drvdata(hdev);

    if (report->id != 0x30 || size < 12)
        return 0;

    y = ((data[7] >> 4) | ((unsigned int)data[8] << 4)) & 0x0fff;
    y = 0x0fff - y;
    data[7] = (data[7] & 0x0f) | ((y & 0x0f) << 4);
    data[8] = (y >> 4) & 0xff;

    ry = ((data[10] >> 4) | ((unsigned int)data[11] << 4)) & 0x0fff;
    ry = 0x0fff - ry;
    data[10] = (data[10] & 0x0f) | ((ry & 0x0f) << 4);
    data[11] = (ry >> 4) & 0xff;

    /* D-pad hat: byte 4 (data[5]) bits bit0=Dwn bit1=Up bit2=Right bit3=Left. */
    if (drvdata && drvdata->gamepad_dev) {
        u8 b = data[5];
        int dx = ((b >> 2) & 1) - ((b >> 3) & 1);  /* right +1, left -1 */
        int dy = ((b >> 0) & 1) - ((b >> 1) & 1);  /* down +1, up -1 */
        input_report_abs(drvdata->gamepad_dev, ABS_HAT0X, dx);
        input_report_abs(drvdata->gamepad_dev, ABS_HAT0Y, dy);
    }

    /* Newest IMU sample: 6x int16 LE at data[37..48] (full 49-byte report). */
    if (drvdata && drvdata->imu_dev && size >= 49) {
        s16 accel[3], gyro[3];
        int i;

        for (i = 0; i < 3; i++) {
            accel[i] = get_unaligned_le16(&data[37 + i * 2]);
            gyro[i]  = get_unaligned_le16(&data[43 + i * 2]);
        }

        input_report_abs(drvdata->imu_dev, ABS_X, accel[0]);
        input_report_abs(drvdata->imu_dev, ABS_Y, accel[1]);
        input_report_abs(drvdata->imu_dev, ABS_Z, accel[2]);
        input_report_abs(drvdata->imu_dev, ABS_RX, gyro[0]);
        input_report_abs(drvdata->imu_dev, ABS_RY, gyro[1]);
        input_report_abs(drvdata->imu_dev, ABS_RZ, gyro[2]);

        drvdata->imu_timestamp_us += 8000; /* ~8 ms per BT report */
        input_event(drvdata->imu_dev, EV_MSC, MSC_TIMESTAMP,
                    drvdata->imu_timestamp_us);
        input_sync(drvdata->imu_dev);
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

/*
 * Parse the Bluetooth BD_ADDR from hdev->uniq (set by hidp to the remote
 * address, e.g. "9C:54:00:4B:8F:A7" via %pMR) and return true if it belongs
 * to the N-SL SW001 OUI block.
 */
static bool nsl_sw001_matches_oui(struct hid_device *hdev)
{
    unsigned long byte;
    int i, parsed = 0;
    const char *s = hdev->uniq;

    if (!s || !*s)
        return false;

    for (i = 0; i < 6 && *s; i++) {
        byte = simple_strtoul(s, NULL, 16);
        if (byte > 0xff)
            return false;

        if (i == 0 && byte != NSL_SW001_OUI0)
            return false;
        if (i == 1 && byte != NSL_SW001_OUI1)
            return false;
        if (i == 2 && byte != NSL_SW001_OUI2)
            return false;

        parsed++;
        while (*s && *s != ':')
            s++;
        if (*s == ':')
            s++;
    }

    return parsed == 6;
}

/*
 * Create a separate input device for the IMU, mirroring hid-nintendo /
 * hid-sony so that SDL's Linux evdev sensor backend can associate it with
 * the gamepad node (via shared uniq == controller MAC) and expose
 * SDL_SENSOR_ACCEL/GYRO.  Accel is ABS_X/Y/Z, gyro is ABS_RX/RY/RZ.
 */
static int nsl_sw001_imu_input_create(struct nsl_sw001_data *drvdata,
                                      struct hid_device *hdev)
{
    struct input_dev *imu_dev;
    char *name;
    int ret;

    imu_dev = devm_input_allocate_device(&hdev->dev);
    if (!imu_dev)
        return -ENOMEM;

    name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "%s IMU", hdev->name);
    if (!name)
        return -ENOMEM;

    imu_dev->name = name;
    imu_dev->id.bustype = hdev->bus;
    imu_dev->id.vendor = hdev->vendor;
    imu_dev->id.product = hdev->product;
    imu_dev->id.version = hdev->version;
    imu_dev->uniq = hdev->uniq;   /* shared with gamepad: SDL links them */
    imu_dev->phys = hdev->phys;

    __set_bit(INPUT_PROP_ACCELEROMETER, imu_dev->propbit);
    __set_bit(EV_ABS, imu_dev->evbit);
    __set_bit(EV_MSC, imu_dev->evbit);
    __set_bit(MSC_TIMESTAMP, imu_dev->mscbit);

    /* Accelerometer: ABS_X/Y/Z */
    input_set_abs_params(imu_dev, ABS_X,
                         -NSL_IMU_MAX_ACCEL_MAG, NSL_IMU_MAX_ACCEL_MAG,
                         NSL_IMU_FUZZ, 0);
    input_set_abs_params(imu_dev, ABS_Y,
                         -NSL_IMU_MAX_ACCEL_MAG, NSL_IMU_MAX_ACCEL_MAG,
                         NSL_IMU_FUZZ, 0);
    input_set_abs_params(imu_dev, ABS_Z,
                         -NSL_IMU_MAX_ACCEL_MAG, NSL_IMU_MAX_ACCEL_MAG,
                         NSL_IMU_FUZZ, 0);
    input_abs_set_res(imu_dev, ABS_X, NSL_IMU_ACCEL_RES_PER_G);
    input_abs_set_res(imu_dev, ABS_Y, NSL_IMU_ACCEL_RES_PER_G);
    input_abs_set_res(imu_dev, ABS_Z, NSL_IMU_ACCEL_RES_PER_G);

    /* Gyroscope: ABS_RX/RY/RZ */
    input_set_abs_params(imu_dev, ABS_RX,
                         -NSL_IMU_MAX_GYRO_MAG, NSL_IMU_MAX_GYRO_MAG,
                         NSL_IMU_FUZZ, 0);
    input_set_abs_params(imu_dev, ABS_RY,
                         -NSL_IMU_MAX_GYRO_MAG, NSL_IMU_MAX_GYRO_MAG,
                         NSL_IMU_FUZZ, 0);
    input_set_abs_params(imu_dev, ABS_RZ,
                         -NSL_IMU_MAX_GYRO_MAG, NSL_IMU_MAX_GYRO_MAG,
                         NSL_IMU_FUZZ, 0);
    input_abs_set_res(imu_dev, ABS_RX, NSL_IMU_GYRO_RES_PER_DPS);
    input_abs_set_res(imu_dev, ABS_RY, NSL_IMU_GYRO_RES_PER_DPS);
    input_abs_set_res(imu_dev, ABS_RZ, NSL_IMU_GYRO_RES_PER_DPS);

    ret = input_register_device(imu_dev);
    if (ret)
        return ret;

    drvdata->imu_dev = imu_dev;
    return 0;
}

static int nsl_sw001_probe(struct hid_device *hdev,
                           const struct hid_device_id *id)
{
    struct nsl_sw001_data *drvdata;
    int ret;

    /*
     * The id_table matches any 057e:2009 Bluetooth device, but not all of
     * them are N-SL SW001 units.  Only claim devices whose Bluetooth MAC
     * starts with the SW001 OUI (9C:54:00); return -ENODEV so the core
     * falls through to the next driver for everything else.
     */
    if (!nsl_sw001_matches_oui(hdev)) {
        hid_info(hdev, "N-SL SW001: not a 9C:54:00 OUI device, skipping\n");
        return -ENODEV;
    }

    drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
    if (!drvdata)
        return -ENOMEM;
    hid_set_drvdata(hdev, drvdata);

    hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

    ret = hid_parse(hdev);
    if (ret) {
        hid_err(hdev, "HID parse failed: %d\n", ret);
        return ret;
    }

    /*
     * Deliberately do NOT pass HID_CONNECT_HIDRAW.  We only want the evdev
     * (HIDINPUT) node plus our driver callbacks (HID_CONNECT_DRIVER).  The
     * SW001 cannot speak the Nintendo Switch protocol, and Steam Input's own
     * bundled HIDAPI driver grabs the device over hidraw when it identifies
     * as 057e:2009, producing a scrambled button mapping.  With no hidraw
     * node, Steam Input cannot take the HIDAPI path and must fall back to
     * our evdev node, which already exposes standard Linux gamepad codes
     * (identical to hid-nintendo's genuine Pro Controller layout).
     */
    ret = hid_hw_start(hdev, HID_CONNECT_HIDINPUT | HID_CONNECT_DRIVER);
    if (ret) {
        hid_err(hdev, "HID hardware start failed: %d\n", ret);
        return ret;
    }

    ret = nsl_sw001_imu_input_create(drvdata, hdev);
    if (ret) {
        hid_err(hdev, "IMU input device creation failed: %d\n", ret);
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
    .raw_event = nsl_sw001_raw_event,
    .input_mapping = nsl_sw001_input_mapping,
    .input_configured = nsl_sw001_input_configured,
};

module_hid_driver(nsl_sw001_driver);

MODULE_DESCRIPTION("HID quirk for N-SL SW001 Nintendo-style controller");
MODULE_AUTHOR("morallo with OpenCode");
MODULE_LICENSE("GPL");
