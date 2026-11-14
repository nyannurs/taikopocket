/* Registers of the Taiko Pocket core, as seen by the firmware.
 * LiteX CSRs of the base SoC (from gateware/litex/build/.../csr.h) and the
 * Taiko peripheral (gateware/taiko/rtl/taiko_top.sv) at 0x8000_0000. */
#ifndef TP_TAIKO_REGS_H
#define TP_TAIKO_REGS_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ---- base SoC ---- */
#define CSR_BASE 0xF0000000u
#define AUDIO_BUFFER_FLUSH REG32(CSR_BASE + 0x008)
#define BRIDGE_REQUEST_READ REG32(CSR_BASE + 0x800)
#define BRIDGE_REQUEST_WRITE REG32(CSR_BASE + 0x804)
#define BRIDGE_REQUEST_OPENFILE REG32(CSR_BASE + 0x80C)
#define BRIDGE_SLOT_ID REG32(CSR_BASE + 0x810)
#define BRIDGE_DATA_OFFSET REG32(CSR_BASE + 0x814)
#define BRIDGE_TRANSFER_LENGTH REG32(CSR_BASE + 0x818)
#define BRIDGE_RAM_DATA_ADDRESS REG32(CSR_BASE + 0x81C)
#define BRIDGE_FILE_SIZE REG32(CSR_BASE + 0x820)
#define BRIDGE_STATUS REG32(CSR_BASE + 0x824) /* 1 once an operation has finished; cleared by reading */
#define BRIDGE_CURRENT_ADDRESS REG32(CSR_BASE + 0x828)
#define BRIDGE_RESULT_CODE REG32(CSR_BASE + 0x82C)
#define TIMER_UPTIME_LATCH REG32(CSR_BASE + 0x5020)
#define TIMER_UPTIME_HI REG32(CSR_BASE + 0x5024)
#define TIMER_UPTIME_LO REG32(CSR_BASE + 0x5028)
#define UART_RXTX REG32(CSR_BASE + 0x5800)
#define UART_TXFULL REG32(CSR_BASE + 0x5804)
#define FB_DMA_BASE REG32(CSR_BASE + 0x6000)
#define FB_DMA_LENGTH REG32(CSR_BASE + 0x6004)
#define FB_DMA_ENABLE REG32(CSR_BASE + 0x6008)
#define FB_DMA_LOOP REG32(CSR_BASE + 0x6010)
#define FB_DMA_OFFSET REG32(CSR_BASE + 0x6014) /* R: words read of the current pass over the buffer */
#define VTG_ENABLE REG32(CSR_BASE + 0x6800)
#define VTG_HRES REG32(CSR_BASE + 0x6804)
#define VTG_HSYNC_START REG32(CSR_BASE + 0x6808)
#define VTG_HSYNC_END REG32(CSR_BASE + 0x680C)
#define VTG_HSCAN REG32(CSR_BASE + 0x6810)
#define VTG_VRES REG32(CSR_BASE + 0x6814)
#define VTG_VSYNC_START REG32(CSR_BASE + 0x6818)
#define VTG_VSYNC_END REG32(CSR_BASE + 0x681C)
#define VTG_VSCAN REG32(CSR_BASE + 0x6820)
#define CPU_HZ 57120000u

/* ---- Taiko peripheral ---- */
#define TAIKO_BASE 0x80000000u
#define TAIKO(index) REG32(TAIKO_BASE + 4 * (index))
enum {
    TK_ID = 0x00,          /* R   "TPK1" */
    TK_CTRL = 0x01,        /* RW  TK_CTRL_* */
    TK_CMD = 0x02,         /* W   TK_CMD_* (strobes) */
    TK_GAIN = 0x03,        /* RW  song gain, 8.8 */
    TK_VOL_MUSIC = 0x04,   /* RW  8.8 */
    TK_VOL_HITS = 0x05,    /* RW  8.8 */
    TK_START_POS_LO = 0x06, /* W  song position of the first sample, signed 48 bits */
    TK_START_POS_HI = 0x07,
    TK_LEAD_IN = 0x08,     /* W   samples of silence before the stream */
    TK_SKIP = 0x09,        /* W   decoded samples to drop first */
    TK_RING_BASE = 0x0A,   /* W   byte address of the music ring */
    TK_RING_BYTES = 0x0B,  /* W */
    TK_FED = 0x0C,         /* RW  bytes written to the ring since TK_CMD_BEGIN */
    TK_CONSUMED = 0x0D,    /* R   bytes the streamer has read */
    TK_FORMAT = 0x0E,      /* W   bit 0 ADPCM, bit 1 stereo, 31:16 block size in words */
    TK_HIT_ADDR = 0x0F,    /* W   write pointer into the hit sample memory */
    TK_HIT_DATA = 0x10,    /* W   one sample; the pointer advances */
    TK_HIT_LEN0 = 0x11,    /* W   samples in the don slot */
    TK_HIT_LEN1 = 0x12,    /* W   samples in the ka slot */
    TK_HIT_STEP0 = 0x13,   /* W   16.16 playback rate (0x10000 = 48 kHz) */
    TK_HIT_STEP1 = 0x14,
    TK_DON_MASK = 0x15,    /* W   buttons that play don */
    TK_KA_MASK = 0x16,     /* W   buttons that play ka */
    TK_COUNTER_LO = 0x17,  /* R   output sample counter, latched by TK_CMD_LATCH */
    TK_COUNTER_HI = 0x18,
    TK_POS_LO = 0x19,      /* R   song position, latched together with the counter */
    TK_POS_HI = 0x1A,
    TK_STATUS = 0x1B,      /* R   TK_STATUS_* */
    TK_EVENT = 0x1C,       /* R   next button event: bit 31 valid, bit 4 pressed, 3:0 button */
    TK_EVENT_TIME = 0x1D,  /* R   its sample counter (low 32 bits); reading removes the event */
    TK_KEYS = 0x1E,        /* R   buttons held now */
    TK_VIDEO = 0x1F,       /* R   bit 0 in vertical sync, 31:1 frame counter */
    TK_BRIDGE_OFFSET_HI = 0x20, /* RW bits 47:32 of the next data slot read offset */
    TK_OVL_CTRL = 0x22,    /* RW  sprite layer: bit 0 on, bit 1 which list to show from the next frame, 15:8 sprites in it */
    TK_OVL_ADDR = 0x23,    /* W   17:16 memory (0 sheet, 1 palette, 2 cells, 3 sprite lists), 15:0 address,
                              bit 20 address bit 16 (version 4) */
    TK_OVL_DATA = 0x24,    /* W   one entry at that address, which then moves on by one */
    TK_OVL_CLIP = 0x25,    /* W   25:16 and 9:0: clipped cells are drawn in [x0, x1) */
    TK_DEBOUNCE = 0x21,    /* RW  samples a button must stay up before its release counts (0..4095) */
    /* version 4: four hit sounds, each a stretch of the sample memory */
    TK_HIT_SLOT = 0x26,    /* RW  the slot (0..3) the next three registers write to */
    TK_HIT_LEN = 0x27,     /* W   its samples (0: nothing to play) */
    TK_HIT_STEP = 0x28,    /* W   its 16.16 playback rate */
    TK_HIT_BASE = 0x29,    /* W   its first sample in the memory. Reads the memory's size in samples. */
};
#define TK_CTRL_STREAM 1u   /* music comes from the streamer, not from the CPU's audio FIFO writes */
#define TK_CTRL_RUN 2u
#define TK_CTRL_FINISHED 4u /* no more data will be fed: play out, then count on in silence */
#define TK_CTRL_HIT_AUTO 8u /* button presses play hit sounds */
#define TK_CTRL_VIDEO_ON 16u /* 0 after reset: the core outputs black until the picture is set up */
#define TK_CMD_BEGIN 1u
#define TK_CMD_DON 2u
#define TK_CMD_KA 4u
#define TK_CMD_LATCH 8u
#define TK_CMD_CLEAR_OVERFLOW 16u
#define TK_CMD_HIT2 32u /* version 4: start the sound in slot 2, 3 */
#define TK_CMD_HIT3 64u
#define TK_OVL_PAGE_SHOWN 4u /* TK_OVL_CTRL, version 4: the sprite list on screen */
#define TK_STATUS_STARVED 1u
#define TK_STATUS_EVENT 2u
#define TK_STATUS_EVENT_OVERFLOW 4u
#define TK_ID_VALUE 0x54504B31u

/* ---- memory map of the 64 MB SDRAM (see link.ld) ---- */
#define MEM_FRAMEBUFFER0 0x41E00000u
#define MEM_FRAMEBUFFER1 0x41E80000u
#define MEM_MUSIC_RING 0x42000000u
#define MEM_MUSIC_RING_BYTES 0x02000000u /* 32 MB */

#endif
