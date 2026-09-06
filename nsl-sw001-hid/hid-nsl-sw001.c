// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/unaligned.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/math.h>
#include <linux/workqueue.h>

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
 * (drivers/hid/hid-nintendo.c).  The reported values are 16-bit samples
 * that are calibrated (offset + scale from the controller's SPI flash) and
 * normalized to these advertised resolutions so SDL's evdev sensor backend
 * can scale them to physical units.
 */
#define NSL_IMU_MAX_ACCEL_MAG   32767
#define NSL_IMU_ACCEL_RES_PER_G 4096    /* counts per G */
#define NSL_IMU_MAX_GYRO_MAG     32767000 /* (2^16-1)*1000 */
#define NSL_IMU_GYRO_RES_PER_DPS 14247   /* (14.247*1000) counts per deg/s */
#define NSL_IMU_FUZZ             10
#define NSL_IMU_PREC_RANGE_SCALE 1000   /* gyro precision-save scaling */

/*
 * Default IMU calibration (used if reading SPI flash fails), matching the
 * defaults in upstream hid-nintendo.
 */
#define NSL_DFLT_ACCEL_OFFSET 0
#define NSL_DFLT_ACCEL_SCALE  16384
#define NSL_DFLT_GYRO_OFFSET  0
#define NSL_DFLT_GYRO_SCALE   13371

/*
 * Nintendo Switch subcommand protocol (used to read IMU calibration from
 * the controller's SPI flash at connection time).  A subcommand is sent as
 * output report 0x01 (rumble + subcmd); the controller replies with input
 * report 0x21 (subcmd reply).  References:
 *   https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/
 *       blob/master/bluetooth_hid_notes.md
 */
#define NSL_OUTPUT_RUMBLE_AND_SUBCMD   0x01
#define NSL_OUTPUT_RUMBLE_ONLY         0x10
#define NSL_INPUT_SUBCMD_REPLY         0x21
#define NSL_INPUT_IMU_DATA             0x30
#define NSL_SUBCMD_SPI_FLASH_READ      0x10
#define NSL_SUBCMD_ENABLE_VIBRATION    0x48

/*
 * Rumble: the SW001 carries the same linear-actuator motors as a genuine
 * Pro Controller and speaks the Nintendo HD-rumble protocol (verified
 * physically: it vibrates).  Rumble goes out on the dedicated rumble-only
 * output report 0x10 (8-byte rumble field, no subcmd) — the SW001 ACKs
 * subcmd 0x48 (enable vibration) but ignores the rumble field of the
 * rumble+subcmd report 0x01, so 0x10 is required.  It must be resent every
 * ~50ms while an effect is active, since the controller only buzzes while
 * packets keep arriving.  Encoding tables are from dekuNukem's
 * rumble_data_table.md (same as upstream hid-nintendo).
 */
#define NSL_RUMBLE_DATA_SIZE     8
#define NSL_RUMBLE_PERIOD_MS     50
#define NSL_RUMBLE_ZERO_AMP_CNT  5
#define NSL_RUMBLE_MAX_AMP       1003
#define NSL_RUMBLE_DFLT_LOW_HZ   160
#define NSL_RUMBLE_DFLT_HIGH_HZ  320
#define NSL_RUMBLE_MIN_HIGH_HZ   82
#define NSL_RUMBLE_MAX_HIGH_HZ   1253
#define NSL_RUMBLE_MIN_LOW_HZ    41
#define NSL_RUMBLE_MAX_LOW_HZ    626

struct nsl_rumble_freq_data {
    u16 high;
    u8 low;
    u16 freq; /* Hz */
};

struct nsl_rumble_amp_data {
    u8 high;
    u16 low;
    u16 amp;
};

/* Frequency/amplitude lookup tables (dekuNukem rumble_data_table.md). */
static const struct nsl_rumble_freq_data nsl_rumble_frequencies[] = {
    { 0x0000, 0x01,   41 }, { 0x0000, 0x02,   42 }, { 0x0000, 0x03,   43 },
    { 0x0000, 0x04,   44 }, { 0x0000, 0x05,   45 }, { 0x0000, 0x06,   46 },
    { 0x0000, 0x07,   47 }, { 0x0000, 0x08,   48 }, { 0x0000, 0x09,   49 },
    { 0x0000, 0x0A,   50 }, { 0x0000, 0x0B,   51 }, { 0x0000, 0x0C,   52 },
    { 0x0000, 0x0D,   53 }, { 0x0000, 0x0E,   54 }, { 0x0000, 0x0F,   55 },
    { 0x0000, 0x10,   57 }, { 0x0000, 0x11,   58 }, { 0x0000, 0x12,   59 },
    { 0x0000, 0x13,   60 }, { 0x0000, 0x14,   62 }, { 0x0000, 0x15,   63 },
    { 0x0000, 0x16,   64 }, { 0x0000, 0x17,   66 }, { 0x0000, 0x18,   67 },
    { 0x0000, 0x19,   69 }, { 0x0000, 0x1A,   70 }, { 0x0000, 0x1B,   72 },
    { 0x0000, 0x1C,   73 }, { 0x0000, 0x1D,   75 }, { 0x0000, 0x1e,   77 },
    { 0x0000, 0x1f,   78 }, { 0x0000, 0x20,   80 }, { 0x0400, 0x21,   82 },
    { 0x0800, 0x22,   84 }, { 0x0c00, 0x23,   85 }, { 0x1000, 0x24,   87 },
    { 0x1400, 0x25,   89 }, { 0x1800, 0x26,   91 }, { 0x1c00, 0x27,   93 },
    { 0x2000, 0x28,   95 }, { 0x2400, 0x29,   97 }, { 0x2800, 0x2a,   99 },
    { 0x2c00, 0x2b,  102 }, { 0x3000, 0x2c,  104 }, { 0x3400, 0x2d,  106 },
    { 0x3800, 0x2e,  108 }, { 0x3c00, 0x2f,  111 }, { 0x4000, 0x30,  113 },
    { 0x4400, 0x31,  116 }, { 0x4800, 0x32,  118 }, { 0x4c00, 0x33,  121 },
    { 0x5000, 0x34,  123 }, { 0x5400, 0x35,  126 }, { 0x5800, 0x36,  129 },
    { 0x5c00, 0x37,  132 }, { 0x6000, 0x38,  135 }, { 0x6400, 0x39,  137 },
    { 0x6800, 0x3a,  141 }, { 0x6c00, 0x3b,  144 }, { 0x7000, 0x3c,  147 },
    { 0x7400, 0x3d,  150 }, { 0x7800, 0x3e,  153 }, { 0x7c00, 0x3f,  157 },
    { 0x8000, 0x40,  160 }, { 0x8400, 0x41,  164 }, { 0x8800, 0x42,  167 },
    { 0x8c00, 0x43,  171 }, { 0x9000, 0x44,  174 }, { 0x9400, 0x45,  178 },
    { 0x9800, 0x46,  182 }, { 0x9c00, 0x47,  186 }, { 0xa000, 0x48,  190 },
    { 0xa400, 0x49,  194 }, { 0xa800, 0x4a,  199 }, { 0xac00, 0x4b,  203 },
    { 0xb000, 0x4c,  207 }, { 0xb400, 0x4d,  212 }, { 0xb800, 0x4e,  217 },
    { 0xbc00, 0x4f,  221 }, { 0xc000, 0x50,  226 }, { 0xc400, 0x51,  231 },
    { 0xc800, 0x52,  236 }, { 0xcc00, 0x53,  241 }, { 0xd000, 0x54,  247 },
    { 0xd400, 0x55,  252 }, { 0xd800, 0x56,  258 }, { 0xdc00, 0x57,  263 },
    { 0xe000, 0x58,  269 }, { 0xe400, 0x59,  275 }, { 0xe800, 0x5a,  281 },
    { 0xec00, 0x5b,  287 }, { 0xf000, 0x5c,  293 }, { 0xf400, 0x5d,  300 },
    { 0xf800, 0x5e,  306 }, { 0xfc00, 0x5f,  313 }, { 0x0001, 0x60,  320 },
    { 0x0401, 0x61,  327 }, { 0x0801, 0x62,  334 }, { 0x0c01, 0x63,  341 },
    { 0x1001, 0x64,  349 }, { 0x1401, 0x65,  357 }, { 0x1801, 0x66,  364 },
    { 0x1c01, 0x67,  372 }, { 0x2001, 0x68,  381 }, { 0x2401, 0x69,  389 },
    { 0x2801, 0x6a,  397 }, { 0x2c01, 0x6b,  406 }, { 0x3001, 0x6c,  415 },
    { 0x3401, 0x6d,  424 }, { 0x3801, 0x6e,  433 }, { 0x3c01, 0x6f,  443 },
    { 0x4001, 0x70,  452 }, { 0x4401, 0x71,  461 }, { 0x4801, 0x72,  470 },
    { 0x4c01, 0x73,  479 }, { 0x5001, 0x74,  488 }, { 0x5401, 0x75,  497 },
    { 0x5801, 0x76,  506 }, { 0x5c01, 0x77,  515 }, { 0x6001, 0x78,  524 },
    { 0x6401, 0x79,  533 }, { 0x6801, 0x7a,  542 }, { 0x6c01, 0x7b,  551 },
    { 0x7001, 0x7c,  560 }, { 0x7401, 0x7d,  569 }, { 0x7801, 0x7e,  578 },
    { 0x7c01, 0x7f,  587 }, { 0x8001, 0x00,  596 }, { 0x8401, 0x00,  605 },
    { 0x8801, 0x00,  614 }, { 0x8c01, 0x00,  623 }, { 0x9001, 0x00,  632 },
    { 0x9401, 0x00,  641 }, { 0x9801, 0x00,  650 }, { 0x9c01, 0x00,  659 },
    { 0xa001, 0x00,  668 }, { 0xa401, 0x00,  677 }, { 0xa801, 0x00,  686 },
    { 0xac01, 0x00,  695 }, { 0xb001, 0x00,  704 }, { 0xb401, 0x00,  713 },
    { 0xb801, 0x00,  722 }, { 0xbc01, 0x00,  731 }, { 0xc001, 0x00,  740 },
    { 0xc401, 0x00,  749 }, { 0xc801, 0x00,  758 }, { 0xcc01, 0x00,  767 },
    { 0xd001, 0x00,  776 }, { 0xd401, 0x00,  785 }, { 0xd801, 0x00,  794 },
    { 0xdc01, 0x00,  803 }, { 0xe001, 0x00,  812 }, { 0xe401, 0x00,  821 },
    { 0xe801, 0x00,  830 }, { 0xec01, 0x00,  839 }, { 0xf001, 0x00,  848 },
    { 0xf401, 0x00,  857 }, { 0xf801, 0x00,  866 }, { 0xfc01, 0x00,  875 },
};

static const struct nsl_rumble_amp_data nsl_rumble_amplitudes[] = {
    /* high, low, amp */
    { 0x00, 0x0040,    0 },
    { 0x02, 0x8040,   10 }, { 0x04, 0x0041,   12 }, { 0x06, 0x8041,   14 },
    { 0x08, 0x0042,   17 }, { 0x0a, 0x8042,   20 }, { 0x0c, 0x0043,   24 },
    { 0x0e, 0x8043,   28 }, { 0x10, 0x0044,   33 }, { 0x12, 0x8044,   40 },
    { 0x14, 0x0045,   47 }, { 0x16, 0x8045,   56 }, { 0x18, 0x0046,   67 },
    { 0x1a, 0x8046,   80 }, { 0x1c, 0x0047,   95 }, { 0x1e, 0x8047,  112 },
    { 0x20, 0x0048,  117 }, { 0x22, 0x8048,  123 }, { 0x24, 0x0049,  128 },
    { 0x26, 0x8049,  134 }, { 0x28, 0x004a,  140 }, { 0x2a, 0x804a,  146 },
    { 0x2c, 0x004b,  152 }, { 0x2e, 0x804b,  159 }, { 0x30, 0x004c,  166 },
    { 0x32, 0x804c,  173 }, { 0x34, 0x004d,  181 }, { 0x36, 0x804d,  189 },
    { 0x38, 0x004e,  198 }, { 0x3a, 0x804e,  206 }, { 0x3c, 0x004f,  215 },
    { 0x3e, 0x804f,  225 }, { 0x40, 0x0050,  230 }, { 0x42, 0x8050,  235 },
    { 0x44, 0x0051,  240 }, { 0x46, 0x8051,  245 }, { 0x48, 0x0052,  251 },
    { 0x4a, 0x8052,  256 }, { 0x4c, 0x0053,  262 }, { 0x4e, 0x8053,  268 },
    { 0x50, 0x8054,  273 }, { 0x52, 0x0054,  279 }, { 0x54, 0x8054,  286 },
    { 0x56, 0x0055,  292 }, { 0x58, 0x8055,  298 }, { 0x5a, 0x0056,  305 },
    { 0x5c, 0x8056,  311 }, { 0x5e, 0x0057,  318 }, { 0x60, 0x8057,  325 },
    { 0x62, 0x0058,  332 }, { 0x64, 0x8058,  340 }, { 0x66, 0x0059,  347 },
    { 0x68, 0x8059,  355 }, { 0x6a, 0x005a,  362 }, { 0x6c, 0x805a,  370 },
    { 0x6e, 0x805b,  378 }, { 0x70, 0x005c,  387 }, { 0x72, 0x805c,  395 },
    { 0x74, 0x005d,  404 }, { 0x76, 0x805d,  413 }, { 0x78, 0x005e,  422 },
    { 0x7a, 0x805e,  431 }, { 0x7c, 0x005f,  440 }, { 0x7e, 0x805f,  450 },
    { 0x80, 0x0060,  460 }, { 0x82, 0x8060,  470 }, { 0x84, 0x0061,  480 },
    { 0x86, 0x8061,  491 }, { 0x88, 0x0062,  501 }, { 0x8a, 0x8062,  512 },
    { 0x8c, 0x0063,  524 }, { 0x8e, 0x8063,  535 }, { 0x90, 0x0064,  547 },
    { 0x92, 0x8064,  559 }, { 0x94, 0x0065,  571 }, { 0x96, 0x8065,  584 },
    { 0x98, 0x0066,  596 }, { 0x9a, 0x8066,  609 }, { 0x9c, 0x0067,  623 },
    { 0x9e, 0x8067,  636 }, { 0xa0, 0x0068,  650 }, { 0xa2, 0x8068,  665 },
    { 0xa4, 0x0069,  679 }, { 0xa6, 0x8069,  694 }, { 0xa8, 0x006a,  709 },
    { 0xaa, 0x806a,  725 }, { 0xac, 0x006b,  741 }, { 0xae, 0x806b,  757 },
    { 0xb0, 0x006c,  773 }, { 0xb2, 0x806c,  790 }, { 0xb4, 0x006d,  808 },
    { 0xb6, 0x806d,  825 }, { 0xb8, 0x006e,  843 }, { 0xba, 0x806e,  862 },
    { 0xbc, 0x006f,  881 }, { 0xbe, 0x806f,  900 }, { 0xc0, 0x0070,  920 },
    { 0xc2, 0x8070,  940 }, { 0xc4, 0x0071,  960 }, { 0xc6, 0x8071,  981 },
    { 0xc8, 0x0072, NSL_RUMBLE_MAX_AMP }
};

/* Minimum gap between BT output reports, mirroring hid-nintendo's
 * JC_SUBCMD_RATE_LIMITER_BT_MS.  Sending too fast disconnects the clone. */
#define NSL_OUTPUT_MIN_DELTA_MS  60

/*
 * SPI read retry policy, derived from the Android 17 capture
 * (btsnoop_hci_android17.log): the clone often ignores the first 0x10
 * subcommands after connect (0x8012/0x801d reads got no reply at t=0.18s
 * and were resent at +1s and +2s before being abandoned), while the
 * working 0x6020 read at t=4.33s succeeded on the first try.  Mirror that:
 * retry up to NSL_SPI_READ_RETRIES times, spaced ~1s apart.
 */
#define NSL_SPI_READ_RETRIES        3
#define NSL_SPI_READ_RETRY_DELAY_MS 1000

/* SPI flash addresses of factory/user IMU calibration data */
#define NSL_IMU_CAL_FCT_ADDR           0x6020
#define NSL_IMU_CAL_FCT_END            0x6037
#define NSL_IMU_CAL_DATA_SIZE          (NSL_IMU_CAL_FCT_END - NSL_IMU_CAL_FCT_ADDR + 1)
#define NSL_IMU_CAL_USR_MAGIC_ADDR     0x8026
#define NSL_IMU_CAL_USR_MAGIC_SIZE     2
#define NSL_IMU_CAL_USR_DATA_ADDR      0x8028
#define NSL_CAL_USR_MAGIC_0            0xB2
#define NSL_CAL_USR_MAGIC_1            0xA1

/* Size of the largest subcmd reply we can receive (input report + 35) */
#define NSL_MAX_RESP_SIZE              64

struct nsl_sw001_data {
    /* Separate IMU input device (accel ABS_X/Y/Z, gyro ABS_RX/RY/RZ) */
    struct input_dev *imu_dev;
    /* Gamepad input device (from hid-input), used for the D-pad hat */
    struct input_dev *gamepad_dev;
    /* Synthetic per-report timestamp counter (us) for MSC_TIMESTAMP */
    s64 imu_timestamp_us;

    /* Synchronous subcmd I/O */
    struct mutex output_mutex;
    wait_queue_head_t wait;
    spinlock_t lock;
    bool received_resp;
    u8 subcmd_ack_match;
    u8 subcmd_num;
    u8 input_buf[NSL_MAX_RESP_SIZE];

    /* IMU calibration data (offset + scale per axis, + precomputed divs) */
    s16 accel_offset[3];
    s16 accel_scale[3];
    s16 gyro_offset[3];
    s16 gyro_scale[3];
    s32 accel_divisor[3];
    s32 gyro_divisor[3];

    /* Rumble state (output report 0x10 rumble field, resent ~50ms) */
    struct hid_device *hdev;
    u8 rumble_data[NSL_RUMBLE_DATA_SIZE];
    unsigned int rumble_msecs;
    u16 rumble_ll_freq;
    u16 rumble_lh_freq;
    u16 rumble_rl_freq;
    u16 rumble_rh_freq;
    unsigned short rumble_zero_countdown;
    bool rumble_active;
    /* Rumble output is sent from a workqueue, never from raw_event: the BT
     * HIDP output path can block on the L2CAP socket lock, so sending
     * synchronously from the input-report context deadlocks. */
    struct workqueue_struct *rumble_wq;
    struct work_struct rumble_work;

    /* Last BT output report timestamp, to throttle sends (see
     * nsl_enforce_output_rate). */
    unsigned long last_output_msecs;
};

/* Output report payload: 0x01 output_id + packet count + 8-byte rumble */
struct nsl_subcmd_request {
    u8 output_id;
    u8 packet_num;
    u8 rumble_data[8];
    u8 subcmd_id;
    u8 data[];
} __packed;

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

    /*
     * Input report (report id 0x21, 49 bytes): the subcommand reply.
     * Declared as all-Const so hid-input produces no controls for it; its
     * only purpose is to make hid-core instantiate a hid_report for report
     * id 0x21.  Without a declared report, hid_get_report() returns NULL
     * and hid_input_report() drops the byte stream BEFORE .raw_event is
     * called, so nsl_sw001_send_subcmd() would always time out (-110).
     * nsl_sw001_raw_event() inspects and forwards the reply to the waiter.
     */
    0x85, 0x21,       /* Report ID (0x21) */
    0x75, 0x08,
    0x95, 0x30,       /* Report Count (48) -- timer, state, rumble, ack, data */
    0x81, 0x01,       /* Input (Const) */

    /*
     * Output report (report id 0x01, 16 bytes): the pack + rumble +
     * subcommand report.  We never let hid-input interpret it (it is sent
     * only by nsl_sw001_send_subcmd() with nsl_sw001_spi_flash_read()).
     * The size matches the Android 17 capture: every working subcmd output
     * report is SHORT (report id + pack counter + 8-byte rumble + subcmd +
     * data, 16 bytes for an SPI flash read), never the genuine controller's
     * 49-byte padded form.
     */
    0x85, 0x01,       /* Report ID (0x01) */
    0x75, 0x08,
    0x95, 0x0f,       /* Report Count (15) -- pack + 8 rumble + subcmd + data */
    0x91, 0x01,       /* Output (Const) */

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
static void nsl_queue_rumble(struct nsl_sw001_data *drvdata);

static int nsl_sw001_raw_event(struct hid_device *hdev,
                                struct hid_report *report,
                                u8 *data, int size)
{
    unsigned int y, ry;
    struct nsl_sw001_data *drvdata = hid_get_drvdata(hdev);

    /*
     * Subcmd reply (0x21): wake any synchronous waiter that sent a
     * subcommand and is blocked in wait_event_timeout(). Copy the whole
     * reply into input_buf for the caller to parse.  Return 0 so the
     * report still flows through hid-input handling unchanged.
     */
    if (data[0] == NSL_INPUT_SUBCMD_REPLY && drvdata) {
        unsigned long flags;

        spin_lock_irqsave(&drvdata->lock, flags);
        if (!drvdata->received_resp) {
            memcpy(drvdata->input_buf, data,
                   min(size, (int)NSL_MAX_RESP_SIZE));
            drvdata->received_resp = true;
            spin_unlock_irqrestore(&drvdata->lock, flags);
            wake_up(&drvdata->wait);
        } else {
            spin_unlock_irqrestore(&drvdata->lock, flags);
        }
        return 0;
    }

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
        s16 raw_accel[3], raw_gyro[3];
        s32 accel[3], gyro[3];
        int i;

        /*
         * Extract raw 16-bit samples, then apply the controller's factory
         * calibration (read from SPI flash at connect time) exactly like
         * hid-nintendo.  Gyro subtracts the per-axis offset and scales;
         * accel only scales (offset subtraction was found to hurt accuracy).
         * The result is normalized to the advertised absinfo resolution.
         */
        for (i = 0; i < 3; i++) {
            raw_accel[i] = get_unaligned_le16(&data[37 + i * 2]);
            raw_gyro[i]  = get_unaligned_le16(&data[43 + i * 2]);
        }

        for (i = 0; i < 3; i++) {
            gyro[i] = mult_frac(NSL_IMU_PREC_RANGE_SCALE *
                                (raw_gyro[i] - drvdata->gyro_offset[i]),
                                drvdata->gyro_scale[i],
                                drvdata->gyro_divisor[i]);
            accel[i] = ((s32)raw_accel[i] * drvdata->accel_scale[i]) /
                       drvdata->accel_divisor[i];
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

    /*
     * Resend rumble every ~50ms while the effect is held (rumble_active)
     * or during the trailing zero-amp countdown used to stop the motors.
     * The controller only buzzes while 0x10 rumble packets keep arriving,
     * so this periodic resend from the streaming 0x30 reports is what
     * sustains a held effect (the Switch streams continuously like this).
     */
    if (drvdata) {
        unsigned long msecs = jiffies_to_msecs(jiffies);
        unsigned long flags;

        spin_lock_irqsave(&drvdata->lock, flags);
        if ((msecs - drvdata->rumble_msecs) >= NSL_RUMBLE_PERIOD_MS &&
            (drvdata->rumble_active || drvdata->rumble_zero_countdown > 0)) {
            if (!drvdata->rumble_active && drvdata->rumble_zero_countdown > 0)
                drvdata->rumble_zero_countdown--;
            drvdata->rumble_msecs = msecs;
            spin_unlock_irqrestore(&drvdata->lock, flags);
            nsl_queue_rumble(drvdata);
        } else {
            spin_unlock_irqrestore(&drvdata->lock, flags);
        }
    }

    return 0;
}

/*
 * Throttle BT output reports to >= NSL_OUTPUT_MIN_DELTA_MS apart.  Sending
 * subcommands too fast disconnects the clone (mirrors hid-nintendo's
 * joycon_enforce_subcmd_rate, minus the report-delta and TX-offset
 * refinements).
 */
static void nsl_enforce_output_rate(struct nsl_sw001_data *drvdata)
{
    unsigned long msecs = jiffies_to_msecs(jiffies);
    unsigned long delta = msecs - drvdata->last_output_msecs;

    if (delta < NSL_OUTPUT_MIN_DELTA_MS)
        msleep(NSL_OUTPUT_MIN_DELTA_MS - delta);
    drvdata->last_output_msecs = jiffies_to_msecs(jiffies);
}

/*
 * Send an output report and synchronously wait for the matching subcmd
 * reply (0x21), mirroring hid-nintendo's joycon_hid_send_sync().  No retry:
 * calibration is best-effort (fall back to stock defaults on failure) and
 * this runs on the shared HIDP dev_init worker, which the disconnect path
 * cancel_work_sync()s — the probe must fail fast, not stall teardown.
 * The reply is left in drvdata->input_buf on success.
 */
static int nsl_sw001_hid_send_sync(struct hid_device *hdev,
                                   struct nsl_sw001_data *drvdata,
                                   u8 *data, size_t len, u32 timeout)
{
    u8 *buf;
    int ret;

    buf = kmemdup(data, len, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    nsl_enforce_output_rate(drvdata);
    ret = hid_hw_output_report(hdev, buf, len);
    kfree(buf);
    if (ret < 0) {
        hid_dbg(hdev, "output report failed; ret=%d\n", ret);
        return ret;
    }

    ret = wait_event_timeout(drvdata->wait, drvdata->received_resp,
                             timeout);
    if (!ret) {
        hid_dbg(hdev, "subcmd timed out\n");
        memset(drvdata->input_buf, 0, NSL_MAX_RESP_SIZE);
        ret = -ETIMEDOUT;
    } else {
        ret = 0;
    }

    drvdata->received_resp = false;
    return ret;
}

/*
 * The SW001 accepts subcommand output reports at their natural length
 * (output_id + pack counter + 8 rumble + subcmd + data), NOT padded to the
 * genuine controller's 49-byte form.  The Android 17 capture
 * (btsnoop_hci_android17.log) shows every SPI-flash-read subcmd sent as a
 * 16-byte report (A2 01 + 16) and answered, while the polled/report-desc
 * 49-byte form gets no reply (ret=-110).
 */
#define NSL_SUBCMD_REPORT_SIZE (sizeof(struct nsl_subcmd_request) + 5)

/*
 * Build a subcommand output report (output_id 0x01, rolling packet count,
 * zeroed rumble field) at its natural length, and send it with a
 * synchronous wait for the reply.
 */
static int nsl_sw001_send_subcmd(struct hid_device *hdev,
                                 struct nsl_sw001_data *drvdata,
                                 u8 subcmd_id, u8 *data, size_t data_len,
                                 u32 timeout)
{
    struct nsl_subcmd_request *req;
    u8 buffer[NSL_SUBCMD_REPORT_SIZE] = { 0 };
    int ret;

    req = (struct nsl_subcmd_request *)buffer;
    req->output_id = NSL_OUTPUT_RUMBLE_AND_SUBCMD;
    req->packet_num = drvdata->subcmd_num;
    if (++drvdata->subcmd_num > 0xF)
        drvdata->subcmd_num = 0;
    req->subcmd_id = subcmd_id;
    if (data && data_len)
        memcpy(req->data, data, data_len);

    /* Set match fields before sending so raw_event can match the reply */
    drvdata->subcmd_ack_match = subcmd_id;
    drvdata->received_resp = false;

    mutex_lock(&drvdata->output_mutex);
    ret = nsl_sw001_hid_send_sync(hdev, drvdata, buffer,
                                  sizeof(struct nsl_subcmd_request) + data_len,
                                  timeout);
    mutex_unlock(&drvdata->output_mutex);

    return ret;
}

/*
 * Rumble-only output report (report id 0x10, no subcmd): what hid-nintendo
 * sends for force feedback.  The 8-byte rumble field drives both linear
 * motors, 4 bytes each (left then right).  The SW001 ACKs subcmd 0x48
 * (enable vibration) but ignores the rumble field of report 0x01, so the
 * dedicated 0x10 report is required.
 */
struct nsl_rumble_report {
    u8 output_id;      /* 0x10 */
    u8 packet_num;
    u8 rumble_data[NSL_RUMBLE_DATA_SIZE];
} __packed;

static struct nsl_rumble_freq_data nsl_find_rumble_freq(u16 freq)
{
    const int length = ARRAY_SIZE(nsl_rumble_frequencies);
    const struct nsl_rumble_freq_data *data = nsl_rumble_frequencies;
    int i = 0;

    if (freq > data[0].freq) {
        for (i = 1; i < length - 1; i++) {
            if (freq > data[i - 1].freq && freq <= data[i].freq)
                break;
        }
    }
    return data[i];
}

static struct nsl_rumble_amp_data nsl_find_rumble_amp(u16 amp)
{
    const int length = ARRAY_SIZE(nsl_rumble_amplitudes);
    const struct nsl_rumble_amp_data *data = nsl_rumble_amplitudes;
    int i = 0;

    if (amp > data[0].amp) {
        for (i = 1; i < length - 1; i++) {
            if (amp > data[i - 1].amp && amp <= data[i].amp)
                break;
        }
    }
    return data[i];
}

static void nsl_encode_rumble(u8 *data, u16 freq_low, u16 freq_high, u16 amp)
{
    struct nsl_rumble_freq_data freq_l = nsl_find_rumble_freq(freq_low);
    struct nsl_rumble_freq_data freq_h = nsl_find_rumble_freq(freq_high);
    struct nsl_rumble_amp_data amp_d = nsl_find_rumble_amp(amp);

    data[0] = (freq_h.high >> 8) & 0xFF;
    data[1] = (freq_h.high & 0xFF) + amp_d.high;
    data[2] = freq_l.low + ((amp_d.low >> 8) & 0xFF);
    data[3] = amp_d.low & 0xFF;
}

/*
 * Rumble output is sent from a dedicated workqueue (never from raw_event):
 * on the Bluetooth HIDP path hid_hw_output_report can block on the L2CAP
 * socket lock, so doing it synchronously while the input-report handler
 * holds that path's locks deadlocks the HID thread.
 */
static void nsl_rumble_worker(struct work_struct *work);

static void nsl_queue_rumble(struct nsl_sw001_data *drvdata)
{
    if (drvdata->rumble_wq)
        queue_work(drvdata->rumble_wq, &drvdata->rumble_work);
}

static void nsl_rumble_worker(struct work_struct *work)
{
    struct nsl_sw001_data *drvdata =
        container_of(work, struct nsl_sw001_data, rumble_work);
    struct nsl_rumble_report rpt;
    unsigned long flags;

    rpt.output_id = NSL_OUTPUT_RUMBLE_ONLY;
    rpt.packet_num = drvdata->subcmd_num;
    if (++drvdata->subcmd_num > 0xF)
        drvdata->subcmd_num = 0;

    spin_lock_irqsave(&drvdata->lock, flags);
    memcpy(rpt.rumble_data, drvdata->rumble_data, NSL_RUMBLE_DATA_SIZE);
    spin_unlock_irqrestore(&drvdata->lock, flags);

    nsl_enforce_output_rate(drvdata);
    mutex_lock(&drvdata->output_mutex);
    hid_hw_output_report(drvdata->hdev, (u8 *)&rpt, sizeof(rpt));
    mutex_unlock(&drvdata->output_mutex);
}

/*
 * Encode a new rumble state into drvdata->rumble_data and queue a send.
 * amp_r is the right motor, amp_l the left, each 0..65535.  Any nonzero
 * amplitude arms the zero-amp countdown so the periodic resend (from
 * raw_event) keeps the motors buzzing while an effect holds; a zero-amp
 * stop keeps sending silence briefly so the motors fully stop.
 */
static int nsl_set_rumble(struct nsl_sw001_data *drvdata,
                          u16 amp_r, u16 amp_l)
{
    u8 data[NSL_RUMBLE_DATA_SIZE];
    u16 amp;
    unsigned long flags;

    /* right motor at data+4, left motor at data+0 */
    amp = amp_r * (u32)NSL_RUMBLE_MAX_AMP / 65535;
    nsl_encode_rumble(data + 4, drvdata->rumble_rl_freq,
                      drvdata->rumble_rh_freq, amp);
    amp = amp_l * (u32)NSL_RUMBLE_MAX_AMP / 65535;
    nsl_encode_rumble(data, drvdata->rumble_ll_freq,
                      drvdata->rumble_lh_freq, amp);

    spin_lock_irqsave(&drvdata->lock, flags);
    memcpy(drvdata->rumble_data, data, NSL_RUMBLE_DATA_SIZE);
    if (amp_l != 0 || amp_r != 0) {
        drvdata->rumble_active = true;
        drvdata->rumble_zero_countdown = NSL_RUMBLE_ZERO_AMP_CNT;
    } else if (drvdata->rumble_active) {
        /* zero-amp stop: keep sending silence briefly so motors fully stop */
        drvdata->rumble_active = false;
        drvdata->rumble_zero_countdown = NSL_RUMBLE_ZERO_AMP_CNT;
    }
    spin_unlock_irqrestore(&drvdata->lock, flags);

    nsl_queue_rumble(drvdata);
    return 0;
}

static int nsl_sw001_play_effect(struct input_dev *dev, void *data,
                                 struct ff_effect *effect)
{
    struct nsl_sw001_data *drvdata = data;

    if (effect->type != FF_RUMBLE)
        return 0;

    return nsl_set_rumble(drvdata,
                          effect->u.rumble.weak_magnitude,   /* right */
                          effect->u.rumble.strong_magnitude); /* left */
}

/*
 * Read `size` bytes from the controller's SPI flash at `addr` via subcmd
 * 0x10.  On success the data is copied to `reply_data`.
 */
static int nsl_sw001_spi_flash_read(struct hid_device *hdev,
                                    struct nsl_sw001_data *drvdata,
                                    u32 addr, u8 size, u8 *reply_data)
{
    u8 cmd[5];
    int ret;

    put_unaligned_le32(addr, cmd);
    cmd[4] = size;

    for (int attempt = 0; attempt < NSL_SPI_READ_RETRIES; attempt++) {
        ret = nsl_sw001_send_subcmd(hdev, drvdata, NSL_SUBCMD_SPI_FLASH_READ,
                                    cmd, sizeof(cmd), HZ / 4);
        if (!ret)
            break;
        if (attempt < NSL_SPI_READ_RETRIES - 1)
            msleep(NSL_SPI_READ_RETRY_DELAY_MS);
    }
    if (ret) {
        hid_err(hdev, "SPI flash read at 0x%04x failed after %d tries; "
                "ret=%d\n", addr, NSL_SPI_READ_RETRIES, ret);
        return ret;
    }

    /*
     * Input report 0x21 layout:
     *   [0] id = 0x21, [1] timer, [2] battery/con,
     *   [3..5] buttons, [6..8] left stick, [9..11] right stick,
     *   [12] vibrator, [13] ack, [14] subcmd id,
     *   [15..18] address echo, [19] size echo, [20..] SPI data.
     */
    if (drvdata->input_buf[0] != NSL_INPUT_SUBCMD_REPLY) {
        hid_err(hdev, "unexpected reply id 0x%02x\n", drvdata->input_buf[0]);
        return -EIO;
    }
    if (!(drvdata->input_buf[13] & 0x80) ||
        drvdata->input_buf[14] != NSL_SUBCMD_SPI_FLASH_READ) {
        hid_err(hdev, "SPI read NACK or mismatch (ack=0x%02x id=0x%02x)\n",
                drvdata->input_buf[13], drvdata->input_buf[14]);
        return -EIO;
    }
    memcpy(reply_data, &drvdata->input_buf[20], size);
    return 0;
}

/*
 * Read the controller's IMU calibration from SPI flash at connect time.
 * Uses the user calibration if present (magic at 0x8026), otherwise the
 * factory calibration at 0x6020.  On any failure, falls back to the same
 * defaults as hid-nintendo.  Precomputes the scale/offset divisors too.
 */
static void nsl_sw001_read_imu_calibration(struct hid_device *hdev,
                                           struct nsl_sw001_data *drvdata)
{
    u8 cal_data[NSL_IMU_CAL_DATA_SIZE];
    u8 magic[NSL_IMU_CAL_USR_MAGIC_SIZE];
    u16 cal_addr = NSL_IMU_CAL_FCT_ADDR;
    int ret, i;

    ret = nsl_sw001_spi_flash_read(hdev, drvdata, NSL_IMU_CAL_USR_MAGIC_ADDR,
                                   NSL_IMU_CAL_USR_MAGIC_SIZE, magic);
    if (!ret && magic[0] == NSL_CAL_USR_MAGIC_0 &&
        magic[1] == NSL_CAL_USR_MAGIC_1) {
        cal_addr = NSL_IMU_CAL_USR_DATA_ADDR;
        hid_info(hdev, "using user IMU calibration\n");
    } else {
        hid_info(hdev, "using factory IMU calibration\n");
    }

    ret = nsl_sw001_spi_flash_read(hdev, drvdata, cal_addr,
                                   NSL_IMU_CAL_DATA_SIZE, cal_data);
    if (ret) {
        hid_warn(hdev, "IMU cal read failed, using defaults; ret=%d\n", ret);
        for (i = 0; i < 3; i++) {
            drvdata->accel_offset[i] = NSL_DFLT_ACCEL_OFFSET;
            drvdata->accel_scale[i]  = NSL_DFLT_ACCEL_SCALE;
            drvdata->gyro_offset[i]  = NSL_DFLT_GYRO_OFFSET;
            drvdata->gyro_scale[i]   = NSL_DFLT_GYRO_SCALE;
        }
    } else {
        /* 24 bytes: accel offset[3], accel scale[3], gyro offset[3], gyro scale[3] */
        for (i = 0; i < 3; i++) {
            int j = i * 2;
            drvdata->accel_offset[i] = get_unaligned_le16(cal_data + j);
            drvdata->accel_scale[i]  = get_unaligned_le16(cal_data + j + 6);
            drvdata->gyro_offset[i]  = get_unaligned_le16(cal_data + j + 12);
            drvdata->gyro_scale[i]   = get_unaligned_le16(cal_data + j + 18);
        }
        hid_info(hdev,
                 "IMU cal: accel o=[%d,%d,%d] s=[%d,%d,%d] "
                 "gyro o=[%d,%d,%d] s=[%d,%d,%d]\n",
                 drvdata->accel_offset[0], drvdata->accel_offset[1],
                 drvdata->accel_offset[2],
                 drvdata->accel_scale[0], drvdata->accel_scale[1],
                 drvdata->accel_scale[2],
                 drvdata->gyro_offset[0], drvdata->gyro_offset[1],
                 drvdata->gyro_offset[2],
                 drvdata->gyro_scale[0], drvdata->gyro_scale[1],
                 drvdata->gyro_scale[2]);
    }

    for (i = 0; i < 3; i++) {
        drvdata->accel_divisor[i] = drvdata->accel_scale[i] -
                                    drvdata->accel_offset[i];
        drvdata->gyro_divisor[i]  = drvdata->gyro_scale[i] -
                                    drvdata->gyro_offset[i];
        if (!drvdata->accel_divisor[i])
            drvdata->accel_divisor[i] = 1;
        if (!drvdata->gyro_divisor[i])
            drvdata->gyro_divisor[i] = 1;
    }
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

    /* Initialize synchronous subcmd I/O primitives */
    mutex_init(&drvdata->output_mutex);
    init_waitqueue_head(&drvdata->wait);
    spin_lock_init(&drvdata->lock);

    /* Rumble defaults + state */
    drvdata->hdev = hdev;
    drvdata->rumble_ll_freq = NSL_RUMBLE_DFLT_LOW_HZ;
    drvdata->rumble_lh_freq = NSL_RUMBLE_DFLT_HIGH_HZ;
    drvdata->rumble_rl_freq = NSL_RUMBLE_DFLT_LOW_HZ;
    drvdata->rumble_rh_freq = NSL_RUMBLE_DFLT_HIGH_HZ;
    drvdata->rumble_msecs = jiffies_to_msecs(jiffies);
    drvdata->rumble_wq = alloc_workqueue("hid-nsl-sw001-rumble", 0, 0);
    if (!drvdata->rumble_wq) {
        hid_err(hdev, "failed to allocate rumble workqueue\n");
        return -ENOMEM;
    }
    INIT_WORK(&drvdata->rumble_work, nsl_rumble_worker);

    /* Seed calibration with defaults in case the SPI read fails */
    {
        int i;
        for (i = 0; i < 3; i++) {
            drvdata->accel_offset[i] = NSL_DFLT_ACCEL_OFFSET;
            drvdata->accel_scale[i]  = NSL_DFLT_ACCEL_SCALE;
            drvdata->gyro_offset[i]  = NSL_DFLT_GYRO_OFFSET;
            drvdata->gyro_scale[i]   = NSL_DFLT_GYRO_SCALE;
            drvdata->accel_divisor[i] = NSL_DFLT_ACCEL_SCALE -
                                        NSL_DFLT_ACCEL_OFFSET;
            drvdata->gyro_divisor[i]  = NSL_DFLT_GYRO_SCALE -
                                        NSL_DFLT_GYRO_OFFSET;
        }
    }

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

    /* Open the HID device so output reports (subcommands) can be sent. */
    ret = hid_hw_open(hdev);
    if (ret) {
        hid_err(hdev, "HID hardware open failed: %d\n", ret);
        hid_hw_stop(hdev);
        return ret;
    }

    /*
     * Release driver_input_lock mid-probe so incoming reports reach
     * raw_event.  hid_device_probe() holds driver_input_lock for the whole
     * probe; during it __hid_input_report()'s down_trylock() fails and the
     * report is dropped BEFORE raw_event.  The synchronous SPI calibration
     * read below waits for the 0x21 reply inside probe, so without this it
     * can never be delivered and the read always times out (ret=-110).
     * hid-nintendo does the same right after hid_hw_open (line 2249).
     */
    hid_device_io_start(hdev);

    ret = nsl_sw001_imu_input_create(drvdata, hdev);
    if (ret) {
        hid_err(hdev, "IMU input device creation failed: %d\n", ret);
        hid_hw_close(hdev);
        hid_hw_stop(hdev);
        return ret;
    }

    /*
     * Register force-feedback (rumble) on the gamepad node.  gamepad_dev is
     * populated by input_configured during hid_hw_start; play_effect reads
     * drvdata via the memless `data` arg.
     */
    if (drvdata->gamepad_dev) {
        input_set_capability(drvdata->gamepad_dev, EV_FF, FF_RUMBLE);
        ret = input_ff_create_memless(drvdata->gamepad_dev, drvdata,
                                      nsl_sw001_play_effect);
        if (ret) {
            hid_err(hdev, "failed to create FF memless effect; ret=%d\n", ret);
            hid_hw_close(hdev);
            hid_hw_stop(hdev);
            return ret;
        }

        /* Enable the motors (subcmd 0x48 with data 0x01). */
        ret = nsl_sw001_send_subcmd(hdev, drvdata,
                                    NSL_SUBCMD_ENABLE_VIBRATION,
                                    (u8[]){ 0x01 }, 1, HZ / 4);
        if (ret)
            hid_warn(hdev, "failed to enable rumble; ret=%d\n", ret);
        else
            hid_info(hdev, "rumble enabled (vibration subcmd ACKed)\n");
    } else {
        hid_warn(hdev, "no gamepad node, skipping rumble setup\n");
    }

    /*
     * Read the IMU calibration from SPI flash.  Best-effort: short
     * timeouts, no retries, falls back to defaults on any failure so the
     * probe cannot stall the HIDP teardown path.
     */
    nsl_sw001_read_imu_calibration(hdev, drvdata);

    return 0;
}

static void nsl_sw001_remove(struct hid_device *hdev)
{
    struct nsl_sw001_data *drvdata = hid_get_drvdata(hdev);

    if (drvdata) {
        /* Wake any blocked synchronous waiter before tearing down */
        drvdata->received_resp = true;
        wake_up(&drvdata->wait);

        /*
         * The IMU device is registered separately from hid-input; hid core
         * only tears down the gamepad nodes, so unregister it here or it
         * lingers as a zombie evdev node whose uniq/phys point into the
         * freed hid_device (UAF if userspace still holds it open).
         */
        if (drvdata->imu_dev) {
            input_unregister_device(drvdata->imu_dev);
            drvdata->imu_dev = NULL;
        }
    }
    hid_hw_close(hdev);
    hid_hw_stop(hdev);
    /* No more raw_events after hid_hw_stop, so it is safe to flush queued
     * rumble sends (they now fail harmlessly on the stopped device). */
    if (drvdata && drvdata->rumble_wq)
        destroy_workqueue(drvdata->rumble_wq);
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
    .remove = nsl_sw001_remove,
    .report_fixup = nsl_sw001_report_fixup,
    .raw_event = nsl_sw001_raw_event,
    .input_mapping = nsl_sw001_input_mapping,
    .input_configured = nsl_sw001_input_configured,
};

module_hid_driver(nsl_sw001_driver);

MODULE_DESCRIPTION("HID quirk for N-SL SW001 Nintendo-style controller");
MODULE_AUTHOR("morallo with OpenCode");
MODULE_LICENSE("GPL");
