/** @file
 * PS2K - PS/2 keyboard emulation.
 */

/*
 * Copyright (C) 2007-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*
 * References:
 *
 * IBM PS/2 Technical Reference, Keyboards (101- and 102-Key), 1990
 * Keyboard Scan Code Specification, Microsoft, 2000
 *
 * Notes:
 *  - The keyboard never sends partial scan-code sequences; if there isn't enough
 *    room left in the buffer for the entire sequence, the keystroke is discarded
 *    and an overrun code is sent instead.
 *  - Command responses do not disturb stored keystrokes and always have priority.
 *  - Caps Lock and Scroll Lock are normal keys from the keyboard's point of view.
 *    However, Num Lock is not and the keyboard internally tracks its state.
 *  - The way Print Screen works in scan set 1/2 is totally insane.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP   LOG_GROUP_DEV_KBD
#include <VBox/vmm/pdmdev.h>
#include <VBox/err.h>
#include <iprt/assert.h>
#include <iprt/uuid.h>
#include "VBoxDD.h"
#define IN_PS2K
#include "PS2Dev.h"

/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/** @name Keyboard commands sent by the system.
 * @{ */
#define KCMD_LEDS           0xED
#define KCMD_ECHO           0xEE
#define KCMD_INVALID_1      0xEF
#define KCMD_SCANSET        0xF0
#define KCMD_INVALID_2      0xF1
#define KCMD_READ_ID        0xF2
#define KCMD_RATE_DELAY     0xF3
#define KCMD_ENABLE         0xF4
#define KCMD_DFLT_DISABLE   0xF5
#define KCMD_SET_DEFAULT    0xF6
#define KCMD_ALL_TYPEMATIC  0xF7
#define KCMD_ALL_MK_BRK     0xF8
#define KCMD_ALL_MAKE       0xF9
#define KCMD_ALL_TMB        0xFA
#define KCMD_TYPE_MATIC     0xFB
#define KCMD_TYPE_MK_BRK    0xFC
#define KCMD_TYPE_MAKE      0xFD
#define KCMD_RESEND         0xFE
#define KCMD_RESET          0xFF
/** @} */

/** @name Keyboard responses sent to the system.
 * @{ */
#define KRSP_ID1            0xAB
#define KRSP_ID2            0x83
#define KRSP_BAT_OK         0xAA
#define KRSP_BAT_FAIL       0xFC
#define KRSP_ECHO           0xEE
#define KRSP_ACK            0xFA
#define KRSP_RESEND         0xFE
/** @} */

/** @name HID modifier range.
 * @{ */
#define HID_MODIFIER_FIRST  0xE0
#define HID_MODIFIER_LAST   0xE8
/** @} */

/** @name USB HID additional constants
 * @{ */
/** The highest USB usage code reported by VirtualBox. */
#define VBOX_USB_MAX_USAGE_CODE     0xE7
/** The size of an array needed to store all USB usage codes */
#define VBOX_USB_USAGE_ARRAY_SIZE   (VBOX_USB_MAX_USAGE_CODE + 1)
/** @} */

/** @name Modifier key states. Sorted in USB HID code order.
 * @{ */
#define MOD_LCTRL           0x01
#define MOD_LSHIFT          0x02
#define MOD_LALT            0x04
#define MOD_LGUI            0x08
#define MOD_RCTRL           0x10
#define MOD_RSHIFT          0x20
#define MOD_RALT            0x40
#define MOD_RGUI            0x80
/** @} */

/* Default typematic value. */
#define KBD_DFL_RATE_DELAY  0x2B

/** Define a simple PS/2 input device queue. */
#define DEF_PS2Q_TYPE(name, size)   \
     typedef struct {               \
        uint32_t    rpos;           \
        uint32_t    wpos;           \
        uint32_t    cUsed;          \
        uint32_t    cSize;          \
        uint8_t     abQueue[size];  \
     } name

/* Internal keyboard queue sizes. The input queue doesn't need to be
 * extra huge and the command queue only needs to handle a few bytes.
 */
#define KBD_KEY_QUEUE_SIZE         64
#define KBD_CMD_QUEUE_SIZE          4

/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/

/** Scancode translator state.  */
typedef enum {
    SS_IDLE,    /**< Starting state. */
    SS_EXT,     /**< E0 byte was received. */
    SS_EXT1     /**< E1 byte was received. */
} scan_state_t;

/** Typematic state. */
typedef enum {
    KBD_TMS_IDLE    = 0,    /* No typematic key active. */
    KBD_TMS_DELAY   = 1,    /* In the initial delay period. */
    KBD_TMS_REPEAT  = 2,    /* Key repeating at set rate. */
    KBD_TMS_32BIT_HACK = 0x7fffffff
} tmatic_state_t;


DEF_PS2Q_TYPE(KbdKeyQ, KBD_KEY_QUEUE_SIZE);
DEF_PS2Q_TYPE(KbdCmdQ, KBD_CMD_QUEUE_SIZE);
DEF_PS2Q_TYPE(GeneriQ, 1);

/**
 * The PS/2 keyboard instance data.
 */
typedef struct PS2K
{
    /** Pointer to parent device (keyboard controller). */
    R3PTRTYPE(void *)     pParent;
    /** Set if keyboard is enabled ('scans' for input). */
    bool                fScanning;
    /** Set NumLock is on. */
    bool                fNumLockOn;
    /** Selected scan set. */
    uint8_t             u8ScanSet;
    /** Modifier key state. */
    uint8_t             u8Modifiers;
    /** Currently processed command (if any). */
    uint8_t             u8CurrCmd;
    /** Status indicator (LED) state. */
    uint8_t             u8LEDs;
    /** Selected typematic delay/rate. */
    uint8_t             u8Typematic;
    /** Usage code of current typematic key, if any. */
    uint8_t             u8TypematicKey;
    /** Current typematic repeat state. */
    tmatic_state_t      enmTypematicState;
    /** Buffer holding scan codes to be sent to the host. */
    KbdKeyQ             keyQ;
    /** Command response queue (priority). */
    KbdCmdQ             cmdQ;
    /** Currently depressed keys. */
    uint8_t             abDepressedKeys[VBOX_USB_USAGE_ARRAY_SIZE];
    /** Typematic delay in milliseconds. */
    unsigned            uTypematicDelay;
    /** Typematic repeat period in milliseconds. */
    unsigned            uTypematicRepeat;
#if HC_ARCH_BITS == 32
    uint32_t            Alignment0;
#endif
    /** Critical section protecting the state. */
    PDMCRITSECT         KbdCritSect;
    /** Command delay timer - RC Ptr. */
    PTMTIMERRC          pKbdDelayTimerRC;
    /** Typematic timer - RC Ptr. */
    PTMTIMERRC          pKbdTypematicTimerRC;
    /** Command delay timer - R3 Ptr. */
    PTMTIMERR3          pKbdDelayTimerR3;
    /** Typematic timer - R3 Ptr. */
    PTMTIMERR3          pKbdTypematicTimerR3;
    /** Command delay timer - R0 Ptr. */
    PTMTIMERR0          pKbdDelayTimerR0;
    /** Typematic timer - R0 Ptr. */
    PTMTIMERR0          pKbdTypematicTimerR0;

    scan_state_t        XlatState;      //@todo: temporary
    uint32_t            Alignment1;

    /**
     * Keyboard port - LUN#0.
     *
     * @implements  PDMIBASE
     * @implements  PDMIKEYBOARDPORT
     */
    struct
    {
        /** The base interface for the keyboard port. */
        PDMIBASE                            IBase;
        /** The keyboard port base interface. */
        PDMIKEYBOARDPORT                    IPort;

        /** The base interface of the attached keyboard driver. */
        R3PTRTYPE(PPDMIBASE)                pDrvBase;
        /** The keyboard interface of the attached keyboard driver. */
        R3PTRTYPE(PPDMIKEYBOARDCONNECTOR)   pDrv;
    } Keyboard;
} PS2K, *PPS2K;

AssertCompile(PS2K_STRUCT_FILLER >= sizeof(PS2K));

#ifndef VBOX_DEVICE_STRUCT_TESTCASE

/* Key type flags. */
#define KF_E0        0x01    /* E0 prefix. */
#define KF_NB        0x02    /* No break code. */
#define KF_GK        0x04    /* Gray navigation key. */
#define KF_PS        0x08    /* Print Screen key. */
#define KF_PB        0x10    /* Pause/Break key. */
#define KF_NL        0x20    /* Num Lock key. */
#define KF_NS        0x40    /* NumPad '/' key. */

/* Scan Set 3 typematic defaults. */
#define T_U          0x00    /* Unknown value. */
#define T_T          0x01    /* Key is typematic. */
#define T_M          0x02    /* Key is make only. */
#define T_B          0x04    /* Key is make/break. */

/* Special key values. */
#define NONE         0x93    /* No PS/2 scan code returned. */
#define UNAS         0x94    /* No PS/2 scan assigned to key. */
#define RSVD         0x95    /* Reserved, do not use. */
#define UNKN         0x96    /* Translation unknown. */

/* Key definition structure. */
typedef struct {
    uint8_t makeS1;      /* Set 1 make code. */
    uint8_t makeS2;      /* Set 2 make code. */
    uint8_t makeS3;      /* Set 3 make code. */
    uint8_t keyFlags;    /* Key flags. */
    uint8_t keyMatic;    /* Set 3 typematic default. */
} key_def;

/* USB to PS/2 conversion table for regular keys. */
static const   key_def   aPS2Keys[] = {
    /* 00 */ {NONE, NONE, NONE, KF_NB, T_U }, /* Key N/A: No Event */
    /* 01 */ {0xFF, 0x00, 0x00, KF_NB, T_U }, /* Key N/A: Overrun Error */
    /* 02 */ {0xFC, 0xFC, 0xFC, KF_NB, T_U }, /* Key N/A: POST Fail */
    /* 03 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key N/A: ErrorUndefined */
    /* 04 */ {0x1E, 0x1C, 0x1C,     0, T_T }, /* Key  31: a A */
    /* 05 */ {0x30, 0x32, 0x32,     0, T_T }, /* Key  50: b B */
    /* 06 */ {0x2E, 0x21, 0x21,     0, T_T }, /* Key  48: c C */
    /* 07 */ {0x20, 0x23, 0x23,     0, T_T }, /* Key  33: d D */
    /* 08 */ {0x12, 0x24, 0x24,     0, T_T }, /* Key  19: e E */
    /* 09 */ {0x21, 0x2B, 0x2B,     0, T_T }, /* Key  34: f F */
    /* 0A */ {0x22, 0x34, 0x34,     0, T_T }, /* Key  35: g G */
    /* 0B */ {0x23, 0x33, 0x33,     0, T_T }, /* Key  36: h H */
    /* 0C */ {0x17, 0x43, 0x43,     0, T_T }, /* Key  24: i I */
    /* 0D */ {0x24, 0x3B, 0x3B,     0, T_T }, /* Key  37: j J */
    /* 0E */ {0x25, 0x42, 0x42,     0, T_T }, /* Key  38: k K */
    /* 0F */ {0x26, 0x4B, 0x4B,     0, T_T }, /* Key  39: l L */
    /* 10 */ {0x32, 0x3A, 0x3A,     0, T_T }, /* Key  52: m M */
    /* 11 */ {0x31, 0x31, 0x31,     0, T_T }, /* Key  51: n N */
    /* 12 */ {0x18, 0x44, 0x44,     0, T_T }, /* Key  25: o O */
    /* 13 */ {0x19, 0x4D, 0x4D,     0, T_T }, /* Key  26: p P */
    /* 14 */ {0x10, 0x15, 0x15,     0, T_T }, /* Key  17: q Q */
    /* 15 */ {0x13, 0x2D, 0x2D,     0, T_T }, /* Key  20: r R */
    /* 16 */ {0x1F, 0x1B, 0x1B,     0, T_T }, /* Key  32: s S */
    /* 17 */ {0x14, 0x2C, 0x2C,     0, T_T }, /* Key  21: t T */
    /* 18 */ {0x16, 0x3C, 0x3C,     0, T_T }, /* Key  23: u U */
    /* 19 */ {0x2F, 0x2A, 0x2A,     0, T_T }, /* Key  49: v V */
    /* 1A */ {0x11, 0x1D, 0x1D,     0, T_T }, /* Key  18: w W */
    /* 1B */ {0x2D, 0x22, 0x22,     0, T_T }, /* Key  47: x X */
    /* 1C */ {0x15, 0x35, 0x35,     0, T_T }, /* Key  22: y Y */
    /* 1D */ {0x2C, 0x1A, 0x1A,     0, T_T }, /* Key  46: z Z */
    /* 1E */ {0x02, 0x16, 0x16,     0, T_T }, /* Key   2: 1 ! */
    /* 1F */ {0x03, 0x1E, 0x1E,     0, T_T }, /* Key   3: 2 @ */
    /* 20 */ {0x04, 0x26, 0x26,     0, T_T }, /* Key   4: 3 # */
    /* 21 */ {0x05, 0x25, 0x25,     0, T_T }, /* Key   5: 4 $ */
    /* 22 */ {0x06, 0x2E, 0x2E,     0, T_T }, /* Key   6: 5 % */
    /* 23 */ {0x07, 0x36, 0x36,     0, T_T }, /* Key   7: 6 ^ */
    /* 24 */ {0x08, 0x3D, 0x3D,     0, T_T }, /* Key   8: 7 & */
    /* 25 */ {0x09, 0x3E, 0x3E,     0, T_T }, /* Key   9: 8 * */
    /* 26 */ {0x0A, 0x46, 0x46,     0, T_T }, /* Key  10: 9 ( */
    /* 27 */ {0x0B, 0x45, 0x45,     0, T_T }, /* Key  11: 0 ) */
    /* 28 */ {0x1C, 0x5A, 0x5A,     0, T_T }, /* Key  43: Return */
    /* 29 */ {0x01, 0x76, 0x08,     0, T_M }, /* Key 110: Escape */
    /* 2A */ {0x0E, 0x66, 0x66,     0, T_T }, /* Key  15: Backspace */
    /* 2B */ {0x0F, 0x0D, 0x0D,     0, T_T }, /* Key  16: Tab */
    /* 2C */ {0x39, 0x29, 0x29,     0, T_T }, /* Key  61: Space */
    /* 2D */ {0x0C, 0x4E, 0x4E,     0, T_T }, /* Key  12: - _ */
    /* 2E */ {0x0D, 0x55, 0x55,     0, T_T }, /* Key  13: = + */
    /* 2F */ {0x1A, 0x54, 0x54,     0, T_T }, /* Key  27: [ { */
    /* 30 */ {0x1B, 0x5B, 0x5B,     0, T_T }, /* Key  28: ] } */
    /* 31 */ {0x2B, 0x5D, 0x5C,     0, T_T }, /* Key  29: \ | */
    /* 32 */ {0x2B, 0x5D, 0x5D,     0, T_T }, /* Key  42: Europe 1 (Note 2) */
    /* 33 */ {0x27, 0x4C, 0x4C,     0, T_T }, /* Key  40: ; : */
    /* 34 */ {0x28, 0x52, 0x52,     0, T_T }, /* Key  41: ' " */
    /* 35 */ {0x29, 0x0E, 0x0E,     0, T_T }, /* Key   1: ` ~ */
    /* 36 */ {0x33, 0x41, 0x41,     0, T_T }, /* Key  53: , < */
    /* 37 */ {0x34, 0x49, 0x49,     0, T_T }, /* Key  54: . > */
    /* 38 */ {0x35, 0x4A, 0x4A,     0, T_T }, /* Key  55: / ? */
    /* 39 */ {0x3A, 0x58, 0x14,     0, T_B }, /* Key  30: Caps Lock */
    /* 3A */ {0x3B, 0x05, 0x07,     0, T_M }, /* Key 112: F1 */
    /* 3B */ {0x3C, 0x06, 0x0F,     0, T_M }, /* Key 113: F2 */
    /* 3C */ {0x3D, 0x04, 0x17,     0, T_M }, /* Key 114: F3 */
    /* 3D */ {0x3E, 0x0C, 0x1F,     0, T_M }, /* Key 115: F4 */
    /* 3E */ {0x3F, 0x03, 0x27,     0, T_M }, /* Key 116: F5 */
    /* 3F */ {0x40, 0x0B, 0x2F,     0, T_M }, /* Key 117: F6 */
    /* 40 */ {0x41, 0x83, 0x37,     0, T_M }, /* Key 118: F7 */
    /* 41 */ {0x42, 0x0A, 0x3F,     0, T_M }, /* Key 119: F8 */
    /* 42 */ {0x43, 0x01, 0x47,     0, T_M }, /* Key 120: F9 */
    /* 43 */ {0x44, 0x09, 0x4F,     0, T_M }, /* Key 121: F10 */
    /* 44 */ {0x57, 0x78, 0x56,     0, T_M }, /* Key 122: F11 */
    /* 45 */ {0x58, 0x07, 0x5E,     0, T_M }, /* Key 123: F12 */
    /* 46 */ {0x37, 0x7C, 0x57, KF_PS, T_M }, /* Key 124: Print Screen (Note 1) */
    /* 47 */ {0x46, 0x7E, 0x5F,     0, T_M }, /* Key 125: Scroll Lock */
    /* 48 */ {RSVD, RSVD, RSVD, KF_PB, T_M }, /* Key 126: Break (Ctrl-Pause) */
    /* 49 */ {0x52, 0x70, 0x67, KF_GK, T_M }, /* Key  75: Insert (Note 1) */
    /* 4A */ {0x47, 0x6C, 0x6E, KF_GK, T_M }, /* Key  80: Home (Note 1) */
    /* 4B */ {0x49, 0x7D, 0x6F, KF_GK, T_M }, /* Key  85: Page Up (Note 1) */
    /* 4C */ {0x53, 0x71, 0x64, KF_GK, T_T }, /* Key  76: Delete (Note 1) */
    /* 4D */ {0x4F, 0x69, 0x65, KF_GK, T_M }, /* Key  81: End (Note 1) */
    /* 4E */ {0x51, 0x7A, 0x6D, KF_GK, T_M }, /* Key  86: Page Down (Note 1) */
    /* 4F */ {0x4D, 0x74, 0x6A, KF_GK, T_T }, /* Key  89: Right Arrow (Note 1) */
    /* 50 */ {0x4B, 0x6B, 0x61, KF_GK, T_T }, /* Key  79: Left Arrow (Note 1) */
    /* 51 */ {0x50, 0x72, 0x60, KF_GK, T_T }, /* Key  84: Down Arrow (Note 1) */
    /* 52 */ {0x48, 0x75, 0x63, KF_GK, T_T }, /* Key  83: Up Arrow (Note 1) */
    /* 53 */ {0x45, 0x77, 0x76, KF_NL, T_M }, /* Key  90: Num Lock */
    /* 54 */ {0x35, 0x4A, 0x77, KF_NS, T_M }, /* Key  95: Keypad / (Note 1) */
    /* 55 */ {0x37, 0x7C, 0x7E,     0, T_M }, /* Key 100: Keypad * */
    /* 56 */ {0x4A, 0x7B, 0x84,     0, T_M }, /* Key 105: Keypad - */
    /* 57 */ {0x4E, 0x79, 0x7C,     0, T_T }, /* Key 106: Keypad + */
    /* 58 */ {0x1C, 0x5A, 0x79, KF_E0, T_M }, /* Key 108: Keypad Enter */
    /* 59 */ {0x4F, 0x69, 0x69,     0, T_M }, /* Key  93: Keypad 1 End */
    /* 5A */ {0x50, 0x72, 0x72,     0, T_M }, /* Key  98: Keypad 2 Down */
    /* 5B */ {0x51, 0x7A, 0x7A,     0, T_M }, /* Key 103: Keypad 3 PageDn */
    /* 5C */ {0x4B, 0x6B, 0x6B,     0, T_M }, /* Key  92: Keypad 4 Left */
    /* 5D */ {0x4C, 0x73, 0x73,     0, T_M }, /* Key  97: Keypad 5 */
    /* 5E */ {0x4D, 0x74, 0x74,     0, T_M }, /* Key 102: Keypad 6 Right */
    /* 5F */ {0x47, 0x6C, 0x6C,     0, T_M }, /* Key  91: Keypad 7 Home */
    /* 60 */ {0x48, 0x75, 0x75,     0, T_M }, /* Key  96: Keypad 8 Up */
    /* 61 */ {0x49, 0x7D, 0x7D,     0, T_M }, /* Key 101: Keypad 9 PageUp */
    /* 62 */ {0x52, 0x70, 0x70,     0, T_M }, /* Key  99: Keypad 0 Insert */
    /* 63 */ {0x53, 0x71, 0x71,     0, T_M }, /* Key 104: Keypad . Delete */
    /* 64 */ {0x56, 0x61, 0x13,     0, T_T }, /* Key  45: Europe 2 (Note 2) */
    /* 65 */ {0x5D, 0x2F, UNKN, KF_E0, T_U }, /* Key 129: App */
    /* 66 */ {0x5E, 0x37, UNKN, KF_E0, T_U }, /* Key Unk: Keyboard Power */
    /* 67 */ {0x59, 0x0F, UNKN,     0, T_U }, /* Key Unk: Keypad = */
    /* 68 */ {0x64, 0x08, UNKN,     0, T_U }, /* Key Unk: F13 */
    /* 69 */ {0x65, 0x10, UNKN,     0, T_U }, /* Key Unk: F14 */
    /* 6A */ {0x66, 0x18, UNKN,     0, T_U }, /* Key Unk: F15 */
    /* 6B */ {0x67, 0x20, UNKN,     0, T_U }, /* Key Unk: F16 */
    /* 6C */ {0x68, 0x28, UNKN,     0, T_U }, /* Key Unk: F17 */
    /* 6D */ {0x69, 0x30, UNKN,     0, T_U }, /* Key Unk: F18 */
    /* 6E */ {0x6A, 0x38, UNKN,     0, T_U }, /* Key Unk: F19 */
    /* 6F */ {0x6B, 0x40, UNKN,     0, T_U }, /* Key Unk: F20 */
    /* 70 */ {0x6C, 0x48, UNKN,     0, T_U }, /* Key Unk: F21 */
    /* 71 */ {0x6D, 0x50, UNKN,     0, T_U }, /* Key Unk: F22 */
    /* 72 */ {0x6E, 0x57, UNKN,     0, T_U }, /* Key Unk: F23 */
    /* 73 */ {0x76, 0x5F, UNKN,     0, T_U }, /* Key Unk: F24 */
    /* 74 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Execute */
    /* 75 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Help */
    /* 76 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Menu */
    /* 77 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Select */
    /* 78 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Stop */
    /* 79 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Again */
    /* 7A */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Undo */
    /* 7B */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Cut */
    /* 7C */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Copy */
    /* 7D */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Paste */
    /* 7E */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Find */
    /* 7F */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Mute */
    /* 80 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Volume Up */
    /* 81 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Volume Dn */
    /* 82 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Locking Caps Lock */
    /* 83 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Locking Num Lock */
    /* 84 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Locking Scroll Lock */
    /* 85 */ {0x7E, 0x6D, UNKN,     0, T_U }, /* Key Unk: Keypad , (Brazilian Keypad .) */
    /* 86 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Equal Sign */
    /* 87 */ {0x73, 0x51, UNKN,     0, T_U }, /* Key Unk: Keyboard Intl 1 (Ro) */
    /* 88 */ {0x70, 0x13, UNKN,     0, T_U }, /* Key Unk: Keyboard Intl2 (K'kana/H'gana) */
    /* 89 */ {0x7D, 0x6A, UNKN,     0, T_U }, /* Key Unk: Keyboard Intl 2 (Yen) */
    /* 8A */ {0x79, 0x64, UNKN,     0, T_U }, /* Key Unk: Keyboard Intl 4 (Henkan) */
    /* 8B */ {0x7B, 0x67, UNKN,     0, T_U }, /* Key Unk: Keyboard Intl 5 (Muhenkan) */
    /* 8C */ {0x5C, 0x27, UNKN,     0, T_U }, /* Key Unk: Keyboard Intl 6 (PC9800 Pad ,) */
    /* 8D */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Intl 7 */
    /* 8E */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Intl 8 */
    /* 8F */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Intl 9 */
    /* 90 */ {0xF2, 0xF2, UNKN, KF_NB, T_U }, /* Key Unk: Keyboard Lang 1 (Hang'l/Engl) */
    /* 91 */ {0xF1, 0xF1, UNKN, KF_NB, T_U }, /* Key Unk: Keyboard Lang 2 (Hanja) */
    /* 92 */ {0x78, 0x63, UNKN,     0, T_U }, /* Key Unk: Keyboard Lang 3 (Katakana) */
    /* 93 */ {0x77, 0x62, UNKN,     0, T_U }, /* Key Unk: Keyboard Lang 4 (Hiragana) */
    /* 94 */ {0x76, 0x5F, UNKN,     0, T_U }, /* Key Unk: Keyboard Lang 5 (Zen/Han) */
    /* 95 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Lang 6 */
    /* 96 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Lang 7 */
    /* 97 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Lang 8 */
    /* 98 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Lang 9 */
    /* 99 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Alternate Erase */
    /* 9A */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard SysReq/Attention */
    /* 9B */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Cancel */
    /* 9C */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Clear */
    /* 9D */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Prior */
    /* 9E */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Return */
    /* 9F */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Separator */
    /* A0 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Out */
    /* A1 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Oper */
    /* A2 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard Clear/Again */
    /* A3 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard CrSel/Props */
    /* A4 */ {UNAS, UNAS, UNAS,     0, T_U }, /* Key Unk: Keyboard ExSel */
};

/* USB to PS/2 conversion table for modifier keys. */
static const   key_def   aPS2ModKeys[] = {
    /* E0 */ {0x1D, 0x14, 0x11,     0, T_B }, /* Key  58: Left Control */
    /* E1 */ {0x2A, 0x12, 0x12,     0, T_B }, /* Key  44: Left Shift */
    /* E2 */ {0x38, 0x11, 0x19,     0, T_B }, /* Key  60: Left Alt */
    /* E3 */ {0x5B, 0x1F, UNKN, KF_E0, T_U }, /* Key 127: Left GUI */
    /* E4 */ {0x1D, 0x14, 0x58, KF_E0, T_M }, /* Key  64: Right Control */
    /* E5 */ {0x36, 0x59, 0x59,     0, T_B }, /* Key  57: Right Shift */
    /* E6 */ {0x38, 0x11, 0x39, KF_E0, T_M }, /* Key  62: Right Alt */
    /* E7 */ {0x5C, 0x27, UNKN, KF_E0, T_U }, /* Key 128: Right GUI */
};

/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/

/*
 * Because of historical reasons and poor design, VirtualBox internally uses BIOS
 * PC/XT style scan codes to represent keyboard events. Each key press and release is
 * represented as a stream of bytes, typically only one byte but up to four-byte
 * sequences are possible. In the typical case, the GUI front end generates the stream
 * of scan codes which we need to translate back to a single up/down event.
 *
 * This function could possibly live somewhere else.
 */

/** Lookup table for converting PC/XT scan codes to USB HID usage codes. */
static uint8_t aScancode2Hid[] =
{
    0x00, 0x29, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, /* 00-07 */
    0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2a, 0x2b, /* 08-1F */
    0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, /* 10-17 */
    0x12, 0x13, 0x2f, 0x30, 0x28, 0xe0, 0x04, 0x16, /* 18-1F */
    0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33, /* 20-27 */
    0x34, 0x35, 0xe1, 0x31, 0x1d, 0x1b, 0x06, 0x19, /* 28-2F */
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xe5, 0x55, /* 30-37 */
    0xe2, 0x2c, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, /* 38-3F */
    0x3f, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5f, /* 40-47 */
    0x60, 0x61, 0x56, 0x5c, 0x5d, 0x5e, 0x57, 0x59, /* 48-4F */
    0x5a, 0x5b, 0x62, 0x63, 0x00, 0x00, 0x64, 0x44, /* 50-57 */
    0x45, 0x67, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, /* 58-5F */
    0x00, 0x00, 0x00, 0x00, 0x68, 0x69, 0x6a, 0x6b, /* 60-67 */
    0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x00, /* 68-6F */
    0x88, 0x91, 0x90, 0x87, 0x00, 0x00, 0x00, 0x00, /* 70-77 */
    0x00, 0x8a, 0x00, 0x8b, 0x00, 0x89, 0x85, 0x00  /* 78-7F */
};

/** Lookup table for extended scancodes (arrow keys etc.). */
static uint8_t aExtScan2Hid[] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 00-07 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 08-1F */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 10-17 */
    0x00, 0x00, 0x00, 0x00, 0x58, 0xe4, 0x00, 0x00, /* 18-1F */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 20-27 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 28-2F */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x00, 0x46, /* 30-37 */
    /* Sun-specific keys.  Most of the XT codes are made up  */
    0xe6, 0x00, 0x00, 0x75, 0x76, 0x77, 0xA3, 0x78, /* 38-3F */
    0x80, 0x81, 0x82, 0x79, 0x00, 0x00, 0x48, 0x4a, /* 40-47 */
    0x52, 0x4b, 0x00, 0x50, 0x00, 0x4f, 0x00, 0x4d, /* 48-4F */
    0x51, 0x4e, 0x49, 0x4c, 0x00, 0x00, 0x00, 0x00, /* 50-57 */
    0x00, 0x00, 0x00, 0xe3, 0xe7, 0x65, 0x66, 0x00, /* 58-5F */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 60-67 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 68-6F */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 70-77 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* 78-7F */
};

/**
 * Convert a PC scan code to a USB HID usage byte.
 *
 * @param state         Current state of the translator (scan_state_t).
 * @param scanCode      Incoming scan code.
 * @param pUsage        Pointer to usage; high bit set for key up events. The
 *                      contents are only valid if returned state is SS_IDLE.
 *
 * @return scan_state_t New state of the translator.
 */
static scan_state_t ScancodeToHidUsage(scan_state_t state, uint8_t scanCode, uint32_t *pUsage)
{
    uint32_t    keyUp;
    uint8_t     usage;

    Assert(pUsage);

    /* Isolate the scan code and key break flag. */
    keyUp = (scanCode & 0x80) << 24;

    switch (state) {
    case SS_IDLE:
        if (scanCode == 0xE0) {
            state = SS_EXT;
        } else if (scanCode == 0xE1) {
            state = SS_EXT1;
        } else {
            usage = aScancode2Hid[scanCode & 0x7F];
            *pUsage = usage | keyUp;
            /* Remain in SS_IDLE state. */
        }
        break;
    case SS_EXT:
        usage = aExtScan2Hid[scanCode & 0x7F];
        *pUsage = usage | keyUp;
        state = SS_IDLE;
        break;
    case SS_EXT1:
        /* The sequence is E1 1D 45 E1 9D C5. We take the easy way out and remain
         * in the SS_EXT1 state until 45 or C5 is received.
         */
        if ((scanCode & 0x7F) == 0x45) {
            *pUsage = 0x48;
            if (scanCode == 0xC5)
                *pUsage |= keyUp;
            state = SS_IDLE;
        }
        /* Else remain in SS_EXT1 state. */
        break;
    }
    return state;
}

/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/


/**
 * Clear a queue.
 *
 * @param   pQ                  Pointer to the queue.
 */
static void PS2ClearQueue(GeneriQ *pQ)
{
    LogFlowFunc(("Clearing queue %p\n", pQ));
    pQ->wpos  = pQ->rpos;
    pQ->cUsed = 0;
}


/**
 * Add a byte to a queue.
 *
 * @param   pQ                  Pointer to the queue.
 * @param   val                 The byte to store.
 */
static void PS2InsertQueue(GeneriQ *pQ, uint8_t val)
{
    /* Check if queue is full. */
    if (pQ->cUsed >= pQ->cSize)
    {
        LogFlowFunc(("queue %p full (%d entries)\n", pQ, pQ->cUsed));
        return;
    }
    /* Insert data and update circular buffer write position. */
    pQ->abQueue[pQ->wpos] = val;
    if (++pQ->wpos == pQ->cSize)
        pQ->wpos = 0;   /* Roll over. */
    ++pQ->cUsed;
    LogFlowFunc(("inserted 0x%02X into queue %p\n", val, pQ));
}

#ifdef IN_RING3

/**
 * Save a queue state.
 *
 * @param   pSSM                SSM handle to write the state to.
 * @param   pQ                  Pointer to the queue.
 */
static void PS2SaveQueue(PSSMHANDLE pSSM, GeneriQ *pQ)
{
    uint32_t    cItems = pQ->cUsed;
    int         i;

    /* Only save the number of items. Note that the read/write
     * positions aren't saved as they will be rebuilt on load.
     */
    SSMR3PutU32(pSSM, cItems);

    LogFlow(("Storing %d items from queue %p\n", cItems, pQ));

    /* Save queue data - only the bytes actually used (typically zero). */
    for (i = pQ->rpos; cItems-- > 0; i = (i + 1) % pQ->cSize)
        SSMR3PutU8(pSSM, pQ->abQueue[i]);
}

/**
 * Load a queue state.
 *
 * @param   pSSM                SSM handle to read the state from.
 * @param   pQ                  Pointer to the queue.
 *
 * @return  int                 VBox status/error code.
 */
static int PS2LoadQueue(PSSMHANDLE pSSM, GeneriQ *pQ)
{
    int         rc;

    /* On load, always put the read pointer at zero. */
    SSMR3GetU32(pSSM, &pQ->cUsed);

    LogFlow(("Loading %d items to queue %p\n", pQ->cUsed, pQ));

    if (pQ->cUsed > pQ->cSize)
    {
        AssertMsgFailed(("Saved size=%u, actual=%u\n", pQ->cUsed, pQ->cSize));
        return VERR_SSM_DATA_UNIT_FORMAT_CHANGED;
    }

    /* Recalculate queue positions and load data in one go. */
    pQ->rpos = 0;
    pQ->wpos = pQ->cUsed;
    rc = SSMR3GetMem(pSSM, pQ->abQueue, pQ->cUsed);

    return rc;
}

#endif

/**
 * Retrieve a byte from a queue.
 *
 * @param   pQ                  Pointer to the queue.
 * @param   pVal                Pointer to storage for the byte.
 *
 * @return  int                 VINF_TRY_AGAIN if queue is empty,
 *                              VINF_SUCCESS if a byte was read.
 */
int PS2RemoveQueue(GeneriQ *pQ, uint8_t *pVal)
{
    int     rc = VINF_TRY_AGAIN;

    Assert(pVal);
    if (pQ->cUsed)
    {
        *pVal = pQ->abQueue[pQ->rpos];
        if (++pQ->rpos == pQ->cSize)
            pQ->rpos = 0;   /* Roll over. */
        --pQ->cUsed;
        rc = VINF_SUCCESS;
        LogFlowFunc(("removed 0x%02X from queue %p\n", *pVal, pQ));
    } else
        LogFlowFunc(("queue %p empty\n", pQ));
    return rc;
}

/* Convert encoded typematic value to milliseconds. Note that the values are rated
 * with +/- 20% accuracy, so there's no need for high precision.
 */
static void PS2KSetupTypematic(PPS2K pThis, uint8_t val)
{
    int         A, B;
    unsigned    period;

    pThis->u8Typematic = val;
    /* The delay is easy: (1 + value) * 250 ms */
    pThis->uTypematicDelay = (1 + ((val >> 5) & 3)) * 250;
    /* The rate is more complicated: (8 + A) * 2^B * 4.17 ms */
    A = val & 7;
    B = (val >> 3) & 3;
    period = (8 + A) * (1 << B) * 417 / 100;
    pThis->uTypematicRepeat = period;
    Log(("Typematic delay %u ms, repeat period %u ms\n",
            pThis->uTypematicDelay, pThis->uTypematicRepeat));
}

static void PS2KSetDefaults(PPS2K pThis)
{
    LogFlowFunc(("Set keyboard defaults\n"));
    PS2ClearQueue((GeneriQ *)&pThis->keyQ);
    /* Set default Scan Set 3 typematic values. */
    /* Set default typematic rate/delay. */
    PS2KSetupTypematic(pThis, KBD_DFL_RATE_DELAY);
    /* Clear last typematic key?? */
}

/**
 * Receive and process a byte sent by the keyboard controller.
 *
 * @param   pThis               The keyboard.
 * @param   cmd                 The command (or data) byte.
 */
int PS2KByteToKbd(PPS2K pThis, uint8_t cmd)
{
    bool    fHandled = true;

    LogFlowFunc(("new cmd=0x%02X, active cmd=0x%02X\n", cmd, pThis->u8CurrCmd));

    switch (cmd) {
    case KCMD_ECHO:
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ECHO);
        pThis->u8CurrCmd = 0;
        break;
    case KCMD_READ_ID:
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ID1);
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ID2);
        pThis->u8CurrCmd = 0;
        break;
    case KCMD_ENABLE:
        pThis->fScanning = true;
        PS2ClearQueue((GeneriQ *)&pThis->keyQ);
        /* Clear last typematic key?? */
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
        pThis->u8CurrCmd = 0;
        break;
    case KCMD_DFLT_DISABLE:
        pThis->fScanning = false;
        PS2KSetDefaults(pThis);
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
        pThis->u8CurrCmd = 0;
        break;
    case KCMD_SET_DEFAULT:
        PS2KSetDefaults(pThis);
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
        pThis->u8CurrCmd = 0;
        break;
    case KCMD_ALL_TYPEMATIC:
    case KCMD_ALL_MK_BRK:
    case KCMD_ALL_MAKE:
    case KCMD_ALL_TMB:
        //@todo: Set the key types here.
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
        pThis->u8CurrCmd = 0;
        break;
    case KCMD_RESEND:
        pThis->u8CurrCmd = 0;
        break;
    case KCMD_RESET:
        pThis->u8ScanSet = 2;
        PS2KSetDefaults(pThis);
        //@todo: reset more?
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
        pThis->u8CurrCmd = cmd;
        /* Delay BAT completion; the test may take hundreds of ms. */
        TMTimerSetMillies(pThis->CTX_SUFF(pKbdDelayTimer), 2);
        break;
    /* The following commands need a parameter. */
    case KCMD_LEDS:
    case KCMD_SCANSET:
    case KCMD_RATE_DELAY:
    case KCMD_TYPE_MATIC:
    case KCMD_TYPE_MK_BRK:
    case KCMD_TYPE_MAKE:
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
        pThis->u8CurrCmd = cmd;
        break;
    default:
        /* Sending a command instead of a parameter starts the new command. */
        switch (pThis->u8CurrCmd) {
        case KCMD_LEDS:
#ifndef IN_RING3
            return VINF_IOM_R3_IOPORT_WRITE;
#else
            {
                PDMKEYBLEDS enmLeds = PDMKEYBLEDS_NONE;

                if (cmd & 0x01)
                    enmLeds = (PDMKEYBLEDS)(enmLeds | PDMKEYBLEDS_SCROLLLOCK);
                if (cmd & 0x02)
                    enmLeds = (PDMKEYBLEDS)(enmLeds | PDMKEYBLEDS_NUMLOCK);
                if (cmd & 0x04)
                    enmLeds = (PDMKEYBLEDS)(enmLeds | PDMKEYBLEDS_CAPSLOCK);
                pThis->Keyboard.pDrv->pfnLedStatusChange(pThis->Keyboard.pDrv, enmLeds);
                pThis->fNumLockOn = !!(cmd & 0x02); /* Sync internal Num Lock state. */
                PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
                pThis->u8LEDs = cmd;
                pThis->u8CurrCmd = 0;
            }
#endif
            break;
        case KCMD_SCANSET:
            PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
            if (cmd == 0)
                PS2InsertQueue((GeneriQ *)&pThis->cmdQ, pThis->u8ScanSet);
            else if (cmd < 4)
            {
                pThis->u8ScanSet = cmd;
                LogRel(("PS2K: Selected scan set %d.\n", cmd));
            }
            /* Other values are simply ignored. */
            pThis->u8CurrCmd = 0;
            break;
        case KCMD_RATE_DELAY:
            PS2KSetupTypematic(pThis, cmd);
            PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_ACK);
            pThis->u8CurrCmd = 0;
            break;
        default:
            fHandled = false;
        }
        /* Fall through only to handle unrecognized commands. */
        if (fHandled)
            break;

    case KCMD_INVALID_1:
    case KCMD_INVALID_2:
        PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_RESEND);
        pThis->u8CurrCmd = 0;
        break;
    }
    LogFlowFunc(("Active cmd now 0x%02X; updating interrupts\n", pThis->u8CurrCmd));
//    KBCUpdateInterrupts(pThis->pParent);
    return VINF_SUCCESS;
}

/**
 * Send a byte (keystroke or command response) to the keyboard controller.
 *
 * @param   pThis               The keyboard.
 */
int PS2KByteFromKbd(PPS2K pThis, uint8_t *pVal)
{
    int         rc;

    Assert(pVal);

    /* Anything in the command queue has priority over data
     * in the keystroke queue. Additionally, keystrokes are
     * blocked if a command is currently in progress, even if
     * the command queue is empty.
     */
    rc = PS2RemoveQueue((GeneriQ *)&pThis->cmdQ, pVal);
    if (rc != VINF_SUCCESS && !pThis->u8CurrCmd && pThis->fScanning)
        rc = PS2RemoveQueue((GeneriQ *)&pThis->keyQ, pVal);

    LogFlowFunc(("keyboard sends 0x%02x (%svalid data)\n", *pVal, rc == VINF_SUCCESS ? "" : "not "));
    return rc;
}

#ifdef IN_RING3

static int PS2KProcessKeyEvent(PPS2K pThis, uint8_t u8HidCode, bool fKeyDown)
{
    unsigned int    i = 0;
    key_def const   *pKeyDef;
    uint8_t         abCodes[16];

    LogFlowFunc(("key %s: 0x%02x (set %d)\n", fKeyDown ? "down" : "up", u8HidCode, pThis->u8ScanSet));

    /* Find the key definition in somewhat sparse storage. */
    pKeyDef = u8HidCode >= HID_MODIFIER_FIRST ? &aPS2ModKeys[u8HidCode - HID_MODIFIER_FIRST] : &aPS2Keys[u8HidCode];

    /* Some keys are not processed at all; early return. */
    if (pKeyDef->makeS1 == NONE)
    {
        LogFlow(("Skipping key processing.\n"));
        return VINF_SUCCESS;
    }

    /* Handle modifier keys (Ctrl/Alt/Shift/GUI). We need to keep track
     * of their state in addition to sending the scan code.
     */
    if (u8HidCode >= HID_MODIFIER_FIRST)
    {
        unsigned    mod_bit = 1 << (u8HidCode - HID_MODIFIER_FIRST);

        Assert((u8HidCode <= HID_MODIFIER_LAST));
        if (fKeyDown)
            pThis->u8Modifiers |= mod_bit;
        else
            pThis->u8Modifiers &= ~mod_bit;
    }

    /* Toggle NumLock state. */
    if ((pKeyDef->keyFlags & KF_NL) && fKeyDown)
        pThis->fNumLockOn ^= true;

    if (pThis->u8ScanSet == 2)
    {
        /* Handle Scan Set 2 - used almost all the time. */
        abCodes[0] = 0;
        if (fKeyDown)
        {
            /* Process key down event. */
            if (pKeyDef->keyFlags & KF_PB)
            {
                /* Pause/Break sends different data if either Ctrl is held. */
                if (pThis->u8Modifiers & (MOD_LCTRL | MOD_RCTRL))
                    strcpy((char *)abCodes, "\xE0\x7E\xE0\xF0\x7E");
                else
                    strcpy((char *)abCodes, "\xE1\x14\x77\xE1\xF0\x14\xF0\x77");
            }
            else if (pKeyDef->keyFlags & KF_PS)
            {
                /* Print Screen depends on all Ctrl, Shift, *and* Alt! */
                if (pThis->u8Modifiers & (MOD_LALT | MOD_RALT))
                    strcpy((char *)abCodes, "\x84");
                else if (pThis->u8Modifiers & (MOD_LSHIFT | MOD_RSHIFT))
                    strcpy((char *)abCodes, "\xE0\x7C");
                else
                    strcpy((char *)abCodes, "\xE0\x12\xE0\x7C");
            }
            else if (pKeyDef->keyFlags & KF_GK)
            {
                if (pThis->fNumLockOn)
                {
                    if ((pThis->u8Modifiers & (MOD_LSHIFT | MOD_RSHIFT)) == 0)
                        strcpy((char *)abCodes, "\xE0\x12");
                }
                else
                {
                    if (pThis->u8Modifiers & MOD_LSHIFT)
                        strcat((char *)abCodes, "\xE0\xF0\x12");
                    if (pThis->u8Modifiers & MOD_RSHIFT)
                        strcat((char *)abCodes, "\xE0\xF0\x59");
                }
            }
            /* Feed the bytes to the queue if there is room. */
            //@todo: check empty space!
            while (abCodes[i])
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, abCodes[i++]);
            Assert(i < sizeof(abCodes));

            /* Standard processing for regular keys only. */
            if (!(pKeyDef->keyFlags & (KF_PB | KF_PS)))
            {
                if (pKeyDef->keyFlags & (KF_E0 | KF_GK | KF_NS))
                    PS2InsertQueue((GeneriQ *)&pThis->keyQ, 0xE0);
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, pKeyDef->makeS2);
            }
        }
        else if (!(pKeyDef->keyFlags & (KF_NB | KF_PB)))
        {
            /* Process key up event except for keys which produce none. */

            /* Handle Print Screen release. */
            if (pKeyDef->keyFlags & KF_PS)
            {
                /* Undo faked Print Screen state as needed. */
                if (pThis->u8Modifiers & (MOD_LALT | MOD_RALT))
                    strcpy((char *)abCodes, "\xF0\x84");
                else if (pThis->u8Modifiers & (MOD_LSHIFT | MOD_RSHIFT))
                    strcpy((char *)abCodes, "\xE0\xF0\x7C");
                else
                    strcpy((char *)abCodes, "\xE0\xF0\x7C\xE0\xF0\x12");
            }
            else
            {
                /* Process base scan code for less unusual keys. */
                if (pKeyDef->keyFlags & (KF_E0 | KF_GK | KF_NS))
                    PS2InsertQueue((GeneriQ *)&pThis->keyQ, 0xE0);
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, 0xF0);
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, pKeyDef->makeS2);

                /* Restore shift state for gray keys. */
                if (pKeyDef->keyFlags & KF_GK)
                {
                    if (pThis->fNumLockOn)
                    {
                        if ((pThis->u8Modifiers & (MOD_LSHIFT | MOD_RSHIFT)) == 0)
                            strcpy((char *)abCodes, "\xE0\xF0\x12");
                    }
                    else
                    {
                        if (pThis->u8Modifiers & MOD_RSHIFT)
                            strcat((char *)abCodes, "\xE0\x59");
                        if (pThis->u8Modifiers & MOD_LSHIFT)
                            strcat((char *)abCodes, "\xE0\x12");
                    }
                }
            }

            /* Feed any additional bytes to the queue if there is room. */
            //@todo: check empty space!
            while (abCodes[i])
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, abCodes[i++]);
            Assert(i < sizeof(abCodes));
        }
    }
    else if (pThis->u8ScanSet == 1)
    {
        /* Handle Scan Set 1 - similar in complexity to Set 2. */
        if (fKeyDown)
        {
            if (pKeyDef->keyFlags & (KF_E0 | KF_GK | KF_NS | KF_PS))
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, 0xE0);
            PS2InsertQueue((GeneriQ *)&pThis->keyQ, pKeyDef->makeS1);
        }
        else if (!(pKeyDef->keyFlags & (KF_NB | KF_PB))) {
            if (pKeyDef->keyFlags & (KF_E0 | KF_GK | KF_NS | KF_PS))
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, 0xE0);
            PS2InsertQueue((GeneriQ *)&pThis->keyQ, pKeyDef->makeS1 | 0x80);
        }
    }
    else
    {
        /* Handle Scan Set 3 - very straightforward. */
        if (fKeyDown)
        {
            PS2InsertQueue((GeneriQ *)&pThis->keyQ, pKeyDef->makeS3);
        }
        else
        {
            /* Send a key release code unless it's a make only key. */
            //@todo: Look up the current typematic setting, not the default!
            if (pKeyDef->keyMatic != T_M)
            {
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, 0xF0);
                PS2InsertQueue((GeneriQ *)&pThis->keyQ, pKeyDef->makeS3);
            }
        }
    }

    /* Set up or cancel typematic key repeat. */
    if (fKeyDown)
    {
        if (pThis->u8TypematicKey != u8HidCode)
        {
            pThis->enmTypematicState = KBD_TMS_DELAY;
            pThis->u8TypematicKey    = u8HidCode;
            TMTimerSetMillies(pThis->CTX_SUFF(pKbdTypematicTimer), pThis->uTypematicDelay);
            Log(("Typematic delay %u ms, key %02X\n", pThis->uTypematicDelay, u8HidCode));
        }
    }
    else
    {
        pThis->u8TypematicKey    = 0;
        pThis->enmTypematicState = KBD_TMS_IDLE;
        //@todo: Cancel timer right away?
        //@todo: Cancel timer before pushing key up code!?
    }

    /* Poke the KBC to update its state. */
    KBCUpdateInterrupts(pThis->pParent);

    return VINF_SUCCESS;
}

/* Timer handler for emulating typematic keys. Note that only the last key
 * held down repeats (if typematic).
 */
static DECLCALLBACK(void) PS2KTypematicTimer(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    PPS2K pThis = (PS2K *)pvUser; //PDMINS_2_DATA(pDevIns, PS2K *);
    int rc = PDMCritSectEnter(&pThis->KbdCritSect, VERR_SEM_BUSY);
    AssertReleaseRC(rc);

    LogFlowFunc(("Typematic state=%d, key %02X\n", pThis->enmTypematicState, pThis->u8TypematicKey));

    /* If the current typematic key is zero, the repeat was canceled just when
     * the timer was about to run. In that case, do nothing.
     */
    if (pThis->u8TypematicKey)
    {
        if (pThis->enmTypematicState == KBD_TMS_DELAY)
            pThis->enmTypematicState = KBD_TMS_REPEAT;

        if (pThis->enmTypematicState == KBD_TMS_REPEAT)
        {
            PS2KProcessKeyEvent(pThis, pThis->u8TypematicKey, true /* Key down */ );
            TMTimerSetMillies(pThis->CTX_SUFF(pKbdTypematicTimer), pThis->uTypematicRepeat);
        }
    }

    PDMCritSectLeave(&pThis->KbdCritSect);
}

/* The keyboard BAT is specified to take several hundred milliseconds. We need
 * to delay sending the result to the host for at least a tiny little while.
 */
static DECLCALLBACK(void) PS2KDelayTimer(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    PPS2K   pThis = GetPS2KFromDevIns(pDevIns);
    int rc = PDMCritSectEnter(&pThis->KbdCritSect, VERR_SEM_BUSY);
    AssertReleaseRC(rc);

    LogFlowFunc(("Delay timer: cmd %02X\n", pThis->u8CurrCmd));

    Assert(pThis->u8CurrCmd == KCMD_RESET);
    PS2InsertQueue((GeneriQ *)&pThis->cmdQ, KRSP_BAT_OK);
    pThis->fScanning = true;    /* BAT completion enables scanning! */
    pThis->u8CurrCmd = 0;

    //@todo: Might want a PS2KCompleteCommand() to push last response, clear command, and kick the KBC...
    /* Give the KBC a kick. */
    KBCUpdateInterrupts(pThis->pParent);

    PDMCritSectLeave(&pThis->KbdCritSect);
}


/**
 * Debug device info handler. Prints basic keyboard state.
 *
 * @param   pDevIns     Device instance which registered the info.
 * @param   pHlp        Callback functions for doing output.
 * @param   pszArgs     Argument string. Optional and specific to the handler.
 */
static DECLCALLBACK(void) PS2KInfoState(PPDMDEVINS pDevIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    PPS2K   pThis = GetPS2KFromDevIns(pDevIns);
    NOREF(pszArgs);

    pHlp->pfnPrintf(pHlp, "PS/2 Keyboard: scan set %d, scanning %s\n",
                    pThis->u8ScanSet, pThis->fScanning ? "enabled" : "disabled");
    pHlp->pfnPrintf(pHlp, "Active command %02X\n", pThis->u8CurrCmd);
    pHlp->pfnPrintf(pHlp, "LED state %02X, Num Lock %s\n", pThis->u8LEDs,
                    pThis->fNumLockOn ? "on" : "off");
    pHlp->pfnPrintf(pHlp, "Typematic delay %ums, repeat period %ums\n",
                    pThis->uTypematicDelay, pThis->uTypematicRepeat);
    pHlp->pfnPrintf(pHlp, "Command queue: %d items (%d max)\n",
                    pThis->cmdQ.cUsed, pThis->cmdQ.cSize);
    pHlp->pfnPrintf(pHlp, "Input queue  : %d items (%d max)\n",
                    pThis->keyQ.cUsed, pThis->keyQ.cSize);
    if (pThis->enmTypematicState != KBD_TMS_IDLE)
        pHlp->pfnPrintf(pHlp, "Active typematic key %02X (%s)\n", pThis->u8Typematic,
                        pThis->enmTypematicState == KBD_TMS_DELAY ? "delay" : "repeat");
}

/* -=-=-=-=-=- Keyboard: IBase  -=-=-=-=-=- */

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) PS2KQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPS2K pThis = RT_FROM_MEMBER(pInterface, PS2K, Keyboard.IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pThis->Keyboard.IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIKEYBOARDPORT, &pThis->Keyboard.IPort);
    return NULL;
}


/* -=-=-=-=-=- Keyboard: IKeyboardPort  -=-=-=-=-=- */

/**
 * Keyboard event handler.
 *
 * @returns VBox status code.
 * @param   pInterface      Pointer to the keyboard port interface (KBDState::Keyboard.IPort).
 * @param   u32Usage        USB HID usage code with key
 *                          press/release flag.
 */
static DECLCALLBACK(int) PS2KPutEvent(PPDMIKEYBOARDPORT pInterface, uint32_t u32Usage)
{
    PPS2K           pThis = RT_FROM_MEMBER(pInterface, PS2K, Keyboard.IPort);
    uint8_t         u8HidCode;
    bool            fKeyDown;
    bool            fHaveEvent = true;
    int             rc = VINF_SUCCESS;

    /* Extract the usage code and ensure it's valid. */
    fKeyDown = !(u32Usage & 0x80000000);
    u8HidCode = u32Usage & 0xFF;
    AssertReturn(u8HidCode <= VBOX_USB_MAX_USAGE_CODE, VERR_INTERNAL_ERROR);

    if (fKeyDown)
    {
        /* Due to host key repeat, we can get key events for keys which are
         * already depressed. We need to ignore those. */
        if (pThis->abDepressedKeys[u8HidCode])
            fHaveEvent = false;
        pThis->abDepressedKeys[u8HidCode] = 1;
    }
    else
    {
        /* NB: We allow key release events for keys which aren't depressed.
         * That is unlikely to happen and should not cause trouble.
         */
        pThis->abDepressedKeys[u8HidCode] = 0;
    }

    /* Unless this is a new key press/release, don't even bother. */
    if (fHaveEvent)
    {
        rc = PDMCritSectEnter(&pThis->KbdCritSect, VERR_SEM_BUSY);
        AssertReleaseRC(rc);

        rc = PS2KProcessKeyEvent(pThis, u8HidCode, fKeyDown);

        PDMCritSectLeave(&pThis->KbdCritSect);
    }

    return rc;
}

static DECLCALLBACK(int) PS2KPutEventWrapper(PPDMIKEYBOARDPORT pInterface, uint8_t u8KeyCode)
{
    PPS2K       pThis = RT_FROM_MEMBER(pInterface, PS2K, Keyboard.IPort);
    uint32_t    u32Usage = 0;

    LogFlowFunc(("key code %02X\n", u8KeyCode));
    pThis->XlatState = ScancodeToHidUsage(pThis->XlatState, u8KeyCode, &u32Usage);

    if (pThis->XlatState == SS_IDLE)
    {
        /* Stupid Korean key hack: convert a lone break key into a press/release sequence. */
        if (u32Usage == 0x80000090 || u32Usage == 0x80000091)
            PS2KPutEvent(pInterface, u32Usage & ~0x80000000);

        PS2KPutEvent(pInterface, u32Usage);
    }

    return VINF_SUCCESS;
}


/**
 * Attach command.
 *
 * This is called to let the device attach to a driver for a
 * specified LUN.
 *
 * This is like plugging in the keyboard after turning on the
 * system.
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 * @param   iLUN        The logical unit which is being detached.
 * @param   fFlags      Flags, combination of the PDMDEVATT_FLAGS_* \#defines.
 */
int PS2KAttach(PPDMDEVINS pDevIns, PPS2K pThis, unsigned iLUN, uint32_t fFlags)
{
    int         rc;

    /* The LUN must be 0, i.e. keyboard. */
    Assert(iLUN == 0);
    AssertMsgReturn(fFlags & PDM_TACH_FLAGS_NOT_HOT_PLUG,
                    ("PS/2 keyboard does not support hotplugging\n"),
                    VERR_INVALID_PARAMETER);

    LogFlowFunc(("iLUN=%d\n", iLUN));

    rc = PDMDevHlpDriverAttach(pDevIns, iLUN, &pThis->Keyboard.IBase, &pThis->Keyboard.pDrvBase, "Keyboard Port");
    if (RT_SUCCESS(rc))
    {
        pThis->Keyboard.pDrv = PDMIBASE_QUERY_INTERFACE(pThis->Keyboard.pDrvBase, PDMIKEYBOARDCONNECTOR);
        if (!pThis->Keyboard.pDrv)
        {
            AssertLogRelMsgFailed(("LUN #0 doesn't have a keyboard interface! rc=%Rrc\n", rc));
            rc = VERR_PDM_MISSING_INTERFACE;
        }
    }
    else if (rc == VERR_PDM_NO_ATTACHED_DRIVER)
    {
        Log(("%s/%d: warning: no driver attached to LUN #0!\n", pDevIns->pReg->szName, pDevIns->iInstance));
        rc = VINF_SUCCESS;
    }
    else
        AssertLogRelMsgFailed(("Failed to attach LUN #0! rc=%Rrc\n", rc));

    return rc;
}

void PS2KSaveState(PSSMHANDLE pSSM, PPS2K pThis)
{
    uint32_t    cPressed = 0;
    uint32_t    cbTMSSize = 0;

    LogFlowFunc(("Saving PS2K state\n"));

    /* Save the basic keyboard state. */
    SSMR3PutU8(pSSM, pThis->u8CurrCmd);
    SSMR3PutU8(pSSM, pThis->u8LEDs);
    SSMR3PutU8(pSSM, pThis->u8Typematic);
    SSMR3PutU8(pSSM, pThis->u8TypematicKey);
    SSMR3PutU8(pSSM, pThis->u8Modifiers);
    SSMR3PutU8(pSSM, pThis->u8ScanSet);
    SSMR3PutU8(pSSM, pThis->enmTypematicState);
    SSMR3PutBool(pSSM, pThis->fNumLockOn);
    SSMR3PutBool(pSSM, pThis->fScanning);

    /* Save the command and keystroke queues. */
    PS2SaveQueue(pSSM, (GeneriQ *)&pThis->cmdQ);
    PS2SaveQueue(pSSM, (GeneriQ *)&pThis->keyQ);

    /* Save the command delay timer. Note that the typematic repeat
     * timer is *not* saved.
     */
    TMR3TimerSave(pThis->CTX_SUFF(pKbdDelayTimer), pSSM);

    /* Save any pressed keys. This is necessary to avoid "stuck"
     * keys after a restore. Needs two passes.
     */
    for (unsigned i = 0; i < sizeof(pThis->abDepressedKeys); ++i)
        if (pThis->abDepressedKeys[i])
            ++cPressed;

    SSMR3PutU32(pSSM, cPressed);

    for (unsigned i = 0; i < sizeof(pThis->abDepressedKeys); ++i)
        if (pThis->abDepressedKeys[i])
            SSMR3PutU8(pSSM, pThis->abDepressedKeys[i]);

    /* Save the typematic settings for Scan Set 3. */
    SSMR3PutU32(pSSM, cbTMSSize);
    /* Currently not implemented. */
}

int PS2KLoadState(PSSMHANDLE pSSM, PPS2K pThis, uint32_t uVersion)
{
    uint8_t     u8;
    uint32_t    cPressed;
    uint32_t    cbTMSSize;
    int         rc;

    NOREF(uVersion);
    LogFlowFunc(("Loading PS2K state version %u\n", uVersion));

    /* Load the basic keyboard state. */
    SSMR3GetU8(pSSM, &pThis->u8CurrCmd);
    SSMR3GetU8(pSSM, &pThis->u8LEDs);
    SSMR3GetU8(pSSM, &pThis->u8Typematic);
    SSMR3GetU8(pSSM, &pThis->u8TypematicKey);
    SSMR3GetU8(pSSM, &pThis->u8Modifiers);
    SSMR3GetU8(pSSM, &pThis->u8ScanSet);
    SSMR3GetU8(pSSM, &u8);
    pThis->enmTypematicState = (tmatic_state_t)u8;
    SSMR3GetBool(pSSM, &pThis->fNumLockOn);
    SSMR3GetBool(pSSM, &pThis->fScanning);

    do {
        /* Load the command and keystroke queues. */
        rc = PS2LoadQueue(pSSM, (GeneriQ *)&pThis->cmdQ);
        if (RT_FAILURE(rc)) break;
        rc = PS2LoadQueue(pSSM, (GeneriQ *)&pThis->keyQ);
        if (RT_FAILURE(rc)) break;

        /* Load the command delay timer, just in case. */
        rc = TMR3TimerLoad(pThis->CTX_SUFF(pKbdDelayTimer), pSSM);
        if (RT_FAILURE(rc)) break;

        /* Recalculate the typematic delay/rate. */
        PS2KSetupTypematic(pThis, pThis->u8Typematic);

        /* Fake key up events for keys that were held down at the time the state was saved. */
        rc = SSMR3GetU32(pSSM, &cPressed);
        if (RT_FAILURE(rc)) break;

        while (cPressed--)
        {
            rc = SSMR3GetU8(pSSM, &u8);
            if (RT_FAILURE(rc)) break;
            PS2KProcessKeyEvent(pThis, u8, false /* key up */);
        }
        if (RT_FAILURE(rc)) break;

        /* Load typematic settings for Scan Set 3. */
        rc = SSMR3GetU32(pSSM, &cbTMSSize);
        if (RT_FAILURE(rc)) break;

        while (cbTMSSize--)
        {
            rc = SSMR3GetU8(pSSM, &u8);
            if (RT_FAILURE(rc)) break;
        }
    } while (0);

    return rc;
}

void PS2KReset(PPS2K pThis)
{
    LogFlowFunc(("Resetting PS2K\n"));

    pThis->fScanning         = true;
    pThis->u8ScanSet         = 2;
    pThis->u8CurrCmd         = 0;
    pThis->u8Modifiers       = 0;
    pThis->u8TypematicKey    = 0;
    pThis->enmTypematicState = KBD_TMS_IDLE;

    /* Clear queues and any pressed keys. */
    memset(pThis->abDepressedKeys, 0, sizeof(pThis->abDepressedKeys));
    PS2ClearQueue((GeneriQ *)&pThis->cmdQ);
    PS2KSetDefaults(pThis);     /* Also clears keystroke queue. */

    /* Activate the PS/2 keyboard by default. */
    if (pThis->Keyboard.pDrv)
        pThis->Keyboard.pDrv->pfnSetActive(pThis->Keyboard.pDrv, true);
}

void PS2KRelocate(PPS2K pThis, RTGCINTPTR offDelta)
{
    LogFlowFunc(("Relocating PS2K\n"));
    pThis->pKbdDelayTimerRC     = TMTimerRCPtr(pThis->pKbdDelayTimerR3);
    pThis->pKbdTypematicTimerRC = TMTimerRCPtr(pThis->pKbdTypematicTimerR3);
    NOREF(offDelta);
}

int PS2KConstruct(PPDMDEVINS pDevIns, PPS2K pThis, void *pParent, int iInstance)
{
    int     rc;

    LogFlowFunc(("iInstance=%d\n", iInstance));

    pThis->pParent = pParent;

    /* Initialize the queues. */
    pThis->keyQ.cSize = KBD_KEY_QUEUE_SIZE;
    pThis->cmdQ.cSize = KBD_CMD_QUEUE_SIZE;

    pThis->Keyboard.IBase.pfnQueryInterface = PS2KQueryInterface;
    pThis->Keyboard.IPort.pfnPutEvent       = PS2KPutEventWrapper;

    /*
     * Initialize the critical section.
     */
    rc = PDMDevHlpCritSectInit(pDevIns, &pThis->KbdCritSect, RT_SRC_POS, "PS2K#%u", iInstance);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Create the typematic delay/repeat timer. Does not use virtual time!
     */
    PTMTIMER pTimer;
    rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_REAL, PS2KTypematicTimer, pThis,
                                TMTIMER_FLAGS_NO_CRIT_SECT, "PS2K Typematic Timer", &pTimer);
    if (RT_FAILURE (rc))
        return rc;

    pThis->pKbdTypematicTimerR3 = pTimer;
    pThis->pKbdTypematicTimerR0 = TMTimerR0Ptr(pTimer);
    pThis->pKbdTypematicTimerRC = TMTimerRCPtr(pTimer);

    /*
     * Create the command delay timer.
     */
    rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_VIRTUAL, PS2KDelayTimer, pThis,
                                TMTIMER_FLAGS_NO_CRIT_SECT, "PS2K Delay Timer", &pTimer);
    if (RT_FAILURE (rc))
        return rc;

    pThis->pKbdDelayTimerR3 = pTimer;
    pThis->pKbdDelayTimerR0 = TMTimerR0Ptr(pTimer);
    pThis->pKbdDelayTimerRC = TMTimerRCPtr(pTimer);

    /*
     * Register debugger info callbacks.
     */
    PDMDevHlpDBGFInfoRegister(pDevIns, "ps2k", "Display PS/2 keyboard state.", PS2KInfoState);

    return rc;
}

#endif

//@todo: The following should live with the KBC implementation.

/* Table used by the keyboard controller to optionally translate the incoming
 * keyboard data. Note that the translation is designed for essentially taking
 * Scan Set 2 input and producing Scan Set 1 output, but can be turned on and
 * off regardless of what the keyboard is sending.
 */
static uint8_t aAT2PC[128] = {
    0xff,0x43,0x41,0x3f,0x3d,0x3b,0x3c,0x58,0x64,0x44,0x42,0x40,0x3e,0x0f,0x29,0x59,
    0x65,0x38,0x2a,0x70,0x1d,0x10,0x02,0x5a,0x66,0x71,0x2c,0x1f,0x1e,0x11,0x03,0x5b,
    0x67,0x2e,0x2d,0x20,0x12,0x05,0x04,0x5c,0x68,0x39,0x2f,0x21,0x14,0x13,0x06,0x5d,
    0x69,0x31,0x30,0x23,0x22,0x15,0x07,0x5e,0x6a,0x72,0x32,0x24,0x16,0x08,0x09,0x5f,
    0x6b,0x33,0x25,0x17,0x18,0x0b,0x0a,0x60,0x6c,0x34,0x35,0x26,0x27,0x19,0x0c,0x61,
    0x6d,0x73,0x28,0x74,0x1a,0x0d,0x62,0x6e,0x3a,0x36,0x1c,0x1b,0x75,0x2b,0x63,0x76,
    0x55,0x56,0x77,0x78,0x79,0x7a,0x0e,0x7b,0x7c,0x4f,0x7d,0x4b,0x47,0x7e,0x7f,0x6f,
    0x52,0x53,0x50,0x4c,0x4d,0x48,0x01,0x45,0x57,0x4e,0x51,0x4a,0x37,0x49,0x46,0x54
};

/**
 * Convert an AT (Scan Set 2) scancode to PC (Scan Set 1).
 *
 * @param state         Current state of the translator
 *                      (xlat_state_t).
 * @param scanIn        Incoming scan code.
 * @param pScanOut      Pointer to outgoing scan code. The
 *                      contents are only valid if returned
 *                      state is not XS_BREAK.
 *
 * @return xlat_state_t New state of the translator.
 */
int32_t XlateAT2PC(int32_t state, uint8_t scanIn, uint8_t *pScanOut)
{
    uint8_t     scan_in;
    uint8_t     scan_out;

    Assert(pScanOut);
    Assert(state == XS_IDLE || state == XS_BREAK || state == XS_HIBIT);

    /* Preprocess the scan code for a 128-entry translation table. */
    if (scanIn == 0x83)         /* Check for F7 key. */
        scan_in = 0x02;
    else if (scanIn == 0x84)    /* Check for SysRq key. */
        scan_in = 0x7f;
    else
        scan_in = scanIn;

    /* Values 0x80 and above are passed through, except for 0xF0
     * which indicates a key release.
     */
    if (scan_in < 0x80)
    {
        scan_out = aAT2PC[scan_in];
        /* Turn into break code if required. */
        if (state == XS_BREAK || state == XS_HIBIT)
            scan_out |= 0x80;

        state = XS_IDLE;
    }
    else
    {
        /* NB: F0 E0 10 will be translated to E0 E5 (high bit set on last byte)! */
        if (scan_in == 0xF0)        /* Check for break code. */
            state = XS_BREAK;
        else if (state == XS_BREAK)
            state = XS_HIBIT;       /* Remember the break bit. */
        scan_out = scan_in;
    }
    LogFlowFunc(("scan code %02X translated to %02X; new state is %d\n",
                 scanIn, scan_out, state));

    *pScanOut = scan_out;
    return state;
}

#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */
