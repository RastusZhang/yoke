// ============================================================================================
//
//  Copyright (C) 2002 Jeroen Janssen
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
//
// ============================================================================================
//
//  This VBE is part of the VGA Bios specific to the plex86/bochs Emulated VGA card.
//  You can NOT drive any physical vga card with it.
//
// ============================================================================================
//
//  This VBE Bios is based on information taken from :
//   - VESA BIOS EXTENSION (VBE) Core Functions Standard Version 3.0 located at www.vesa.org
//
// ============================================================================================


/*
 * Oracle LGPL Disclaimer: For the avoidance of doubt, except that if any license choice
 * other than GPL or LGPL is available it will apply instead, Oracle elects to use only
 * the Lesser General Public License version 2.1 (LGPLv2) at this time for any software where
 * a choice of LGPL license versions is made available with the language indicating
 * that LGPLv2 or any later version may be used, or where a choice of which version
 * of the LGPL is applied is otherwise unspecified.
 */

// Use VBE new dynamic mode list.  Note that without this option, no
// checks are currently done to make sure that modes fit into the
// framebuffer!
#define VBE_NEW_DYN_LIST

#include <inttypes.h>
#include <stddef.h>
#include "vbe.h"
#include "vgadefs.h"
#include "inlines.h"

// disable VESA/VBE2 check in vbe info
//#define VBE2_NO_VESA_CHECK

// use bytewise i/o (Longhorn beta issue, not in released Vista)
//#define VBE_BYTEWISE_IO

#ifdef VBE_BYTEWISE_IO
    extern void do_out_dx_ax();
    #pragma aux do_out_dx_ax "*";
    extern void out_w(uint16_t port, uint16_t value);
    #pragma aux out_w =     \
        "call do_out_dx_ax" \
        parm [dx] [ax] modify nomemory;
    extern void do_in_ax_dx();
    #pragma aux do_in_ax_dx "*";
    extern uint16_t in_w(uint16_t port);
    #pragma aux in_w =     \
        "call do_in_ax_dx" \
        parm [dx] value [ax] modify nomemory;
#else
    #define out_w       outw
    #define in_w        inw
#endif


/* VESA signatures as integer constants. */
#define SIG_VBE2    0x32454256  /* 'VBE2' */
#define SIG_VESA    0x41534556  /* 'VESA' */


/* Implemented in assembler. */
extern void     __cdecl vga_compat_setup(void);
extern void     dispi_set_enable(uint16_t enable);
extern void     dispi_set_bank(uint16_t bank);
extern uint16_t __cdecl dispi_get_max_bpp(void);
extern void     __cdecl dispi_set_bank_farcall(void);

// The current OEM Software Revision of this VBE Bios
#define VBE_OEM_SOFTWARE_REV 0x0003

// FIXME: 'merge' these (c) etc strings with the vgabios.c strings?
char vbebios_copyright[]        = "VirtualBox VESA BIOS";
char vbebios_vendor_name[]      = VBOX_VENDOR;
char vbebios_product_name[]     = VBOX_PRODUCT " VBE Adapter";
char vbebios_product_revision[] = VBOX_PRODUCT " Version " VBOX_VERSION_STRING;

char vbebios_info_string[]    = "VirtualBox VBE Display Adapter enabled\r\n\r\n";
char no_vbebios_info_string[] = "No VirtualBox VBE support available!\r\n\r\n";

#ifdef VGA_DEBUG
char msg_vbe_init[] = "VirtualBox Version " VBOX_VERSION_STRING " VBE Display Adapter\r\n";
#endif

static void dispi_set_xres(uint16_t xres)
{
#ifdef VGA_DEBUG
    printf("vbe_set_xres: %04x\n", xres);
#endif
    out_w(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    out_w(VBE_DISPI_IOPORT_DATA, xres);
}

static void dispi_set_yres(uint16_t yres)
{
#ifdef VGA_DEBUG
    printf("vbe_set_yres: %04x\n", yres);
#endif
    out_w(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    out_w(VBE_DISPI_IOPORT_DATA, yres);
}

static void dispi_set_bpp(uint16_t bpp)
{
#ifdef VGA_DEBUG
    printf("vbe_set_bpp: %02x\n", bpp);
#endif
    out_w(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    out_w(VBE_DISPI_IOPORT_DATA, bpp);
}

uint16_t in_word(uint16_t port, uint16_t addr)
{
    outw(port, addr);
    return inw(port);
}

#ifdef VBE_NEW_DYN_LIST
uint8_t in_byte(uint16_t port, uint16_t addr)
{
    outw(port, addr);
    return inb(port);
}
#endif

/* Display "chip" identification helpers. */
static uint16_t dispi_get_id(void)
{
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void dispi_set_id(uint16_t chip_id)
{
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
    outw(VBE_DISPI_IOPORT_DATA, chip_id);
}

/* VBE Init - Initialise the VESA BIOS Extension (VBE) support
 * This function does a sanity check on the host side display code interface.
 */
void vbe_init(void)
{
    dispi_set_id(VBE_DISPI_ID0);
    if (dispi_get_id() == VBE_DISPI_ID0) {
        /* VBE support was detected. */
        write_byte(BIOSMEM_SEG, BIOSMEM_VBE_FLAG, 1);
        dispi_set_id(VBE_DISPI_ID4);
    }
#ifdef DEBUG_VGA
    printf(msg_vbe_init);
#endif
}

/* Find the offset of the desired mode, given its number. */
#ifdef VBE_NEW_DYN_LIST
static uint16_t mode_info_find_mode(uint16_t mode, Boolean using_lfb)
{
    uint16_t    sig, vmode, attrs;
    uint16_t    cur_info_ofs;   /* Current offset in mode list. */
    
    /* Read and check the VBE Extra Data signature. */
    sig = in_word(VBE_EXTRA_PORT, 0);
    if (sig != VBEHEADER_MAGIC) {
#ifdef DEBUG_VGA
        printf("Signature NOT found! %x\n", sig);
#endif
        return 0;
    }
    
    cur_info_ofs = sizeof(VBEHeader);
    
    vmode = in_word(VBE_EXTRA_PORT, cur_info_ofs + offsetof(ModeInfoListItem, mode)/*&cur_info->mode*/);
    while (vmode != VBE_VESA_MODE_END_OF_LIST)
    {
        attrs = in_word(VBE_EXTRA_PORT, /*&cur_info->info.ModeAttributes*/cur_info_ofs + offsetof(ModeInfoListItem, info.ModeAttributes) );
        
        if (vmode == mode)
        {
            if (!using_lfb)
                return cur_info_ofs;
            else if (attrs & VBE_MODE_ATTRIBUTE_LINEAR_FRAME_BUFFER_MODE)
                return cur_info_ofs;
            else {
                cur_info_ofs += sizeof(ModeInfoListItem);
                vmode = in_word(VBE_EXTRA_PORT, /*&cur_info->mode*/cur_info_ofs + offsetof(ModeInfoListItem, mode));
            }
        } else {
            cur_info_ofs += sizeof(ModeInfoListItem);
            vmode = in_word(VBE_EXTRA_PORT, /*&cur_info->mode*/cur_info_ofs + offsetof(ModeInfoListItem, mode));
        }
    }
    return 0;
}
#else
static ModeInfoListItem* mode_info_find_mode(uint16_t mode, Boolean using_lfb)
{
    ModeInfoListItem    *cur_info = &mode_info_list;
    
    while (cur_info->mode != VBE_VESA_MODE_END_OF_LIST)
    {
        if (cur_info->mode == mode)
        {
            if (!using_lfb)
                return cur_info;
            else if (cur_info->info.ModeAttributes & VBE_MODE_ATTRIBUTE_LINEAR_FRAME_BUFFER_MODE)
                return cur_info;
            else
                cur_info++;
        } else
            cur_info++;
    }
    return 0;
}
#endif

#ifndef VBOX
; VBE Display Info - Display information on screen about the VBE

vbe_display_info:
  call _vbe_has_vbe_display
  test ax, ax
  jz   no_vbe_flag
  mov  ax, #0xc000
  mov  ds, ax
  mov  si, #_vbebios_info_string
  jmp  _display_string
no_vbe_flag:
  mov  ax, #0xc000
  mov  ds, ax
  mov  si, #_no_vbebios_info_string
  jmp  _display_string
#endif

/** Function 00h - Return VBE Controller Information
 *
 * Input:
 *              AX      = 4F00h
 *              ES:DI   = Pointer to buffer in which to place VbeInfoBlock structure
 *                        (VbeSignature should be VBE2 when VBE 2.0 information is desired and
 *                        the info block is 512 bytes in size)
 * Output:
 *              AX      = VBE Return Status
 *
 */
void vbe_biosfn_return_controller_information(uint16_t STACK_BASED *AX, uint16_t ES, uint16_t DI)
{
    uint16_t            status;
    uint16_t            vbe2_info;
    uint16_t            cur_mode = 0;
    uint16_t            cur_ptr=34;
#ifdef VBE_NEW_DYN_LIST
    uint16_t            cur_info_ofs;
    uint16_t            sig, vmode;
#else
    ModeInfoListItem    *cur_info = &mode_info_list;
#endif
    uint16_t            max_bpp = dispi_get_max_bpp();
    VbeInfoBlock __far  *info_block;

    info_block = ES :> (VbeInfoBlock *)DI;

#ifdef VBE_NEW_DYN_LIST
    /* Read VBE Extra Data signature */
    sig = in_word(VBE_EXTRA_PORT, 0);
    if (sig != VBEHEADER_MAGIC)
    {
        *AX = 0x0100;
#ifdef DEBUG_VGA
        printf("Signature NOT found\n");
#endif
        return;
    }
    cur_info_ofs = sizeof(VBEHeader);
#endif
    status = *AX;

#ifdef VGA_DEBUG
    printf("VBE vbe_biosfn_return_vbe_info ES%x DI%x AX%x\n",ES,DI,status);
#endif

    vbe2_info = 0;

    /* Don't use a local copy of VbeInfoBlock on the stack; it's too big.
     * The Ubuntu 8.04 64 bits splash screen emulator can't handle this.
     */
#ifdef VBE2_NO_VESA_CHECK
#else  /* !VBE2_NO_VESA_CHECK */
    // check for VBE2 signature
    if (info_block->VbeSignature.Sig32 == SIG_VBE2 || info_block->VbeSignature.Sig32 == SIG_VESA)
    {
        vbe2_info = 1;
#ifdef VGA_DEBUG
        printf("VBE correct VESA/VBE2 signature found\n");
#endif
    }
#endif /* !VBE2_NO_VESA_CHECK */

    /* VBE Signature - the compiler will optimize this into something sane. */
    info_block->VbeSignature.SigChr[0] = 'V';
    info_block->VbeSignature.SigChr[1] = 'E';
    info_block->VbeSignature.SigChr[2] = 'S';
    info_block->VbeSignature.SigChr[3] = 'A';

    /* VBE Version supported. */
    info_block->VbeVersion = 0x0200;    /* Version 2.0. */

    /* OEM String. */
    info_block->OemString.Ptr = &vbebios_copyright;

    /* Capabilities if this implementation. */
    info_block->Capabilities[0] = VBE_CAPABILITY_8BIT_DAC;
    info_block->Capabilities[1] = 0;
    info_block->Capabilities[2] = 0;
    info_block->Capabilities[3] = 0;

    /* Video mode list pointer (dynamically generated). */
    info_block->VideoModePtr_Seg = ES;
    info_block->VideoModePtr_Off = DI + 34;

    /* Total controller memory in 64K units. */
    info_block->TotalMemory = in_word(VBE_EXTRA_PORT, 0xffff);

    if (vbe2_info)
    {
        /* OEM information. */
        info_block->OemSoftwareRev     = VBE_OEM_SOFTWARE_REV;
        info_block->OemVendorName.Ptr  = &vbebios_vendor_name;
        info_block->OemProductName.Ptr = &vbebios_product_name;
        info_block->OemProductRev.Ptr  = &vbebios_product_revision;
    }

#ifdef VBE_NEW_DYN_LIST
    do
    {
        uint8_t     data_b;

        data_b = in_byte(VBE_EXTRA_PORT, cur_info_ofs + offsetof(ModeInfoListItem, info.BitsPerPixel) /*&cur_info->info.BitsPerPixel*/);
        if (data_b <= max_bpp)
        {
            vmode = in_word(VBE_EXTRA_PORT, cur_info_ofs + offsetof(ModeInfoListItem, mode)/*&cur_info->mode*/);
#ifdef VGA_DEBUG
            printf("VBE found mode %x => %x\n", vmode, cur_mode);
#endif
            write_word(ES, DI + cur_ptr, vmode);
            cur_mode++;
            cur_ptr+=2;
        }
        cur_info_ofs += sizeof(ModeInfoListItem);
        vmode = in_word(VBE_EXTRA_PORT, cur_info_ofs + offsetof(ModeInfoListItem, mode)/*&cur_info->mode*/);
    } while (vmode != VBE_VESA_MODE_END_OF_LIST);

    // Add vesa mode list terminator
    write_word(ES, DI + cur_ptr, vmode);
#else
    do
    {
        if (cur_info->info.BitsPerPixel <= max_bpp) {
#ifdef VGA_DEBUG
            printf("VBE found mode %x => %x\n", cur_info->mode,cur_mode);
#endif
            write_word(ES, DI + cur_ptr, cur_info->mode);
            cur_mode++;
            cur_ptr += 2;
        }
        cur_info++;
    } while (cur_info->mode != VBE_VESA_MODE_END_OF_LIST);

    // Add vesa mode list terminator
    write_word(ES, DI + cur_ptr, cur_info->mode);
#endif // VBE_NEW_DYN_LIST
    *AX = 0x004F;
}

/** Function 01h - Return VBE Mode Information
 *
 * Input:
 *              AX      = 4F01h
 *              CX      = Mode Number
 *              ES:DI   = Pointer to buffer in which to place ModeInfoBlock structure
 * Output:
 *              AX      = VBE Return Status
 *
 */
void vbe_biosfn_return_mode_information(uint16_t STACK_BASED *AX, uint16_t CX, uint16_t ES, uint16_t DI)
{
    uint16_t            result = 0x0100;
#ifdef VBE_NEW_DYN_LIST
    uint16_t            cur_info_ofs;
#else
    ModeInfoListItem    *cur_info;
#endif
    Boolean             using_lfb;
    uint8_t             win_attr;

#ifdef VGA_DEBUG
    printf("VBE vbe_biosfn_return_mode_information ES%x DI%x CX%x\n",ES,DI,CX);
#endif

    using_lfb = ((CX & VBE_MODE_LINEAR_FRAME_BUFFER) == VBE_MODE_LINEAR_FRAME_BUFFER);
    CX = (CX & 0x1ff);

#ifdef VBE_NEW_DYN_LIST
    cur_info_ofs = mode_info_find_mode(CX, using_lfb);

    if (cur_info_ofs) {
        uint16_t    i;
#else
    cur_info = mode_info_find_mode(CX, using_lfb);

    if (cur_info != 0) {
#endif
#ifdef VGA_DEBUG
        printf("VBE found mode %x\n",CX);
#endif
        memsetb(ES, DI, 0, 256);    // The mode info size is fixed
#ifdef VBE_NEW_DYN_LIST
        for (i = 0; i < sizeof(ModeInfoBlockCompact); i++) {
            uint8_t b;

            b = in_byte(VBE_EXTRA_PORT, cur_info_ofs + offsetof(ModeInfoListItem, info) + i/*(char *)(&(cur_info->info)) + i*/);
            write_byte(ES, DI + i, b);
        }
#else
        memcpyb(ES, DI, 0xc000, &(cur_info->info), sizeof(ModeInfoBlockCompact));
#endif
        win_attr = read_byte(ES, DI + offsetof(ModeInfoBlock, WinAAttributes));
        if (win_attr & VBE_WINDOW_ATTRIBUTE_RELOCATABLE) {
            write_word(ES, DI + offsetof(ModeInfoBlock, WinFuncPtr), (uint16_t)(dispi_set_bank_farcall));
            // If BIOS not at 0xC000 -> boom
            write_word(ES, DI + offsetof(ModeInfoBlock, WinFuncPtr) + 2, 0xC000);
        }
        // Update the LFB physical address which may change at runtime
        out_w(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_FB_BASE_HI);
        write_word(ES, DI + offsetof(ModeInfoBlock, PhysBasePtr) + 2, in_w(VBE_DISPI_IOPORT_DATA));

        result = 0x4f;
    } else {
#ifdef VGA_DEBUG
        printf("VBE *NOT* found mode %x\n",CX);
#endif
        result = 0x100;
    }

    *AX = result;
}

/** Function 02h - Set VBE Mode
 *
 * Input:
 *              AX      = 4F02h
 *              BX      = Desired Mode to set
 *              ES:DI   = Pointer to CRTCInfoBlock structure
 * Output:
 *              AX      = VBE Return Status
 *
 */
void vbe_biosfn_set_mode(uint16_t STACK_BASED *AX, uint16_t BX, uint16_t ES, uint16_t DI)
{
    uint16_t            result;
#ifdef VBE_NEW_DYN_LIST
    uint16_t            cur_info_ofs;
#else
    ModeInfoListItem    *cur_info;
#endif
    Boolean             using_lfb;
    uint8_t             no_clear;
    uint8_t             lfb_flag;

    using_lfb = ((BX & VBE_MODE_LINEAR_FRAME_BUFFER) == VBE_MODE_LINEAR_FRAME_BUFFER);
    lfb_flag  = using_lfb ? VBE_DISPI_LFB_ENABLED : 0;
    no_clear  = ((BX & VBE_MODE_PRESERVE_DISPLAY_MEMORY) == VBE_MODE_PRESERVE_DISPLAY_MEMORY) ? VBE_DISPI_NOCLEARMEM : 0;

    BX = (BX & 0x1ff);

    // check for non vesa mode
    if (BX < VBE_MODE_VESA_DEFINED)
    {
        uint8_t mode;

        dispi_set_enable(VBE_DISPI_DISABLED);
        // call the vgabios in order to set the video mode
        // this allows for going back to textmode with a VBE call (some applications expect that to work)
        mode = (BX & 0xff);
        biosfn_set_video_mode(mode);
        result = 0x4f;
        goto leave;
    }

#ifdef VBE_NEW_DYN_LIST
    cur_info_ofs = mode_info_find_mode(BX, using_lfb);

    if (cur_info_ofs != 0)
    {
        uint16_t    xres, yres;
        uint8_t     bpp;

        xres = in_word(VBE_EXTRA_PORT, cur_info_ofs + offsetof(ModeInfoListItem, info.XResolution) /*&cur_info->info.XResolution*/);
        yres = in_word(VBE_EXTRA_PORT, cur_info_ofs + offsetof(ModeInfoListItem, info.YResolution) /*&cur_info->info.YResolution*/);
        bpp  = in_byte(VBE_EXTRA_PORT, cur_info_ofs + offsetof(ModeInfoListItem, info.BitsPerPixel) /*&cur_info->info.BitsPerPixel*/);

#ifdef VGA_DEBUG
        printf("VBE found mode %x, setting:\n", BX);
        printf("\txres%x yres%x bpp%x\n", xres, yres, bpp);
#endif
#else
    cur_info = mode_info_find_mode(BX, using_lfb);

    if (cur_info != 0)
    {
#ifdef VGA_DEBUG
        printf("VBE found mode %x, setting:\n", BX);
        printf("\txres%x yres%x bpp%x\n",
                cur_info->info.XResolution,
                cur_info->info.YResolution,
                cur_info->info.BitsPerPixel);
#endif
#endif // VBE_NEW_DYN_LIST

        // first disable current mode (when switching between vesa modi)
        dispi_set_enable(VBE_DISPI_DISABLED);

#ifdef VBE_NEW_DYN_LIST
        if (bpp == 4)
#else
        if (cur_info->info.BitsPerPixel == 4)
#endif
        {
            biosfn_set_video_mode(0x6a);
        }

#ifdef VBE_NEW_DYN_LIST
        dispi_set_bpp(bpp);
        dispi_set_xres(xres);
        dispi_set_yres(yres);
#else
        dispi_set_bpp(cur_info->info.BitsPerPixel);
        dispi_set_xres(cur_info->info.XResolution);
        dispi_set_yres(cur_info->info.YResolution);
#endif
        dispi_set_bank(0);
        dispi_set_enable(VBE_DISPI_ENABLED | no_clear | lfb_flag);
        vga_compat_setup();

        write_word(BIOSMEM_SEG,BIOSMEM_VBE_MODE,BX);
        write_byte(BIOSMEM_SEG,BIOSMEM_VIDEO_CTL,(0x60 | no_clear));

        result = 0x4f;
    }
    else
    {
#ifdef VGA_DEBUG
        printf("VBE *NOT* found mode %x\n" , BX);
#endif
        result = 0x100;
    }

leave:
    *AX = result;
}

uint16_t vbe_biosfn_read_video_state_size(void)
{
    return 9 * 2;
}

void vbe_biosfn_save_video_state(uint16_t ES, uint16_t BX)
{
    uint16_t    enable, i;

    outw(VBE_DISPI_IOPORT_INDEX,VBE_DISPI_INDEX_ENABLE);
    enable = inw(VBE_DISPI_IOPORT_DATA);
    write_word(ES, BX, enable);
    BX += 2;
    if (!(enable & VBE_DISPI_ENABLED))
        return;
    for(i = VBE_DISPI_INDEX_XRES; i <= VBE_DISPI_INDEX_Y_OFFSET; i++) {
        if (i != VBE_DISPI_INDEX_ENABLE) {
            outw(VBE_DISPI_IOPORT_INDEX, i);
            write_word(ES, BX, inw(VBE_DISPI_IOPORT_DATA));
            BX += 2;
        }
    }
}


void vbe_biosfn_restore_video_state(uint16_t ES, uint16_t BX)
{
    uint16_t    enable, i;

    enable = read_word(ES, BX);
    BX += 2;

    if (!(enable & VBE_DISPI_ENABLED)) {
        outw(VBE_DISPI_IOPORT_INDEX,VBE_DISPI_INDEX_ENABLE);
        outw(VBE_DISPI_IOPORT_DATA, enable);
    } else {
        outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
        outw(VBE_DISPI_IOPORT_DATA, read_word(ES, BX));
        BX += 2;
        outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
        outw(VBE_DISPI_IOPORT_DATA, read_word(ES, BX));
        BX += 2;
        outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
        outw(VBE_DISPI_IOPORT_DATA, read_word(ES, BX));
        BX += 2;
        outw(VBE_DISPI_IOPORT_INDEX,VBE_DISPI_INDEX_ENABLE);
        outw(VBE_DISPI_IOPORT_DATA, enable);

        for(i = VBE_DISPI_INDEX_BANK; i <= VBE_DISPI_INDEX_Y_OFFSET; i++) {
            outw(VBE_DISPI_IOPORT_INDEX, i);
            outw(VBE_DISPI_IOPORT_DATA, read_word(ES, BX));
            BX += 2;
        }
    }
}

/** Function 04h - Save/Restore State
 *
 * Input:
 *              AX      = 4F04h
 *              DL      = 00h Return Save/Restore State buffer size
 *                        01h Save State
 *                        02h Restore State
 *              CX      = Requested states
 *              ES:BX   = Pointer to buffer (if DL <> 00h)
 * Output:
 *              AX      = VBE Return Status
 *              BX      = Number of 64-byte blocks to hold the state buffer (if DL=00h)
 *
 */
void vbe_biosfn_save_restore_state(uint16_t STACK_BASED *AX, uint16_t CX, uint16_t DX, 
                                   uint16_t ES, uint16_t STACK_BASED *BX)
{
    uint16_t    result, val;

    result = 0x004F;
    switch(GET_DL()) {
    case 0x00:
        val = biosfn_read_video_state_size2(CX);
#ifdef VGA_DEBUG
        printf("VGA state size=%x\n", val);
#endif
        if (CX & 8)
            val += vbe_biosfn_read_video_state_size();
        *BX = (val + 63) / 64;
        break;
    case 0x01:
        val = *BX;
        val = biosfn_save_video_state(CX, ES, val);
#ifdef VGA_DEBUG
        printf("VGA save_state offset=%x\n", val);
#endif
        if (CX & 8)
            vbe_biosfn_save_video_state(ES, val);
        break;
    case 0x02:
        val = *BX;
        val = biosfn_restore_video_state(CX, ES, val);
#ifdef VGA_DEBUG
        printf("VGA restore_state offset=%x\n", val);
#endif
        if (CX & 8)
            vbe_biosfn_restore_video_state(ES, val);
        break;
    default:
        // function failed
        result = 0x100;
        break;
    }
    *AX = result;
}
