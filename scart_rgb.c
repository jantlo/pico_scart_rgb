/**
 * SCART RGB PAL signal generator
 *
 * HARDWARE CONNECTIONS
 *  - GPIO 16 ---> csync
 *  - GPIO 18 ---> 330 ohm resistor ---> VGA Red
 *  - GPIO 19 ---> 330 ohm resistor ---> VGA Green
 *  - GPIO 20 ---> 330 ohm resistor ---> VGA Blue
 *
 */
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include <stdio.h>

#include "csync.pio.h"
#include "rgb.pio.h"

#define SCAN_LINES 304
#define BORDER_TOP_LINES 42
#define BORDER_BOTTOM_LINES 22

#define RES_X 320
#define RES_Y (SCAN_LINES - BORDER_TOP_LINES - BORDER_BOTTOM_LINES)

#define LINE_COUNT (RES_X >> 1) // 2 pixels per byte.
#define FRAMEBUFFER_SIZE (LINE_COUNT * RES_Y)

// I/O pins used
#define CSYNC_PIN 16
#define RED_PIN 18
#define GREEN_PIN 19
#define BLUE_PIN 20

// 1 bit per each color channel, so 8 color.
#define BLACK 0
#define RED 1
#define GREEN 2
#define YELLOW 3
#define BLUE 4
#define MAGENTA 5
#define CYAN 6
#define WHITE 7

static const uint8_t s_colors[8] = {BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE};

static uint8_t s_framebuffer[FRAMEBUFFER_SIZE];
static const uint8_t* s_address_pointer[1] = {s_framebuffer};
static const uint8_t s_border_color = BLACK;

struct control_block_t
{
    uint32_t ctrl;					// Must maps to al1_ctrl
    const volatile void* read_addr; // Must maps to al1_read_addr
    io_wo_32* write_addr;			// Must maps to al1_write_addr
    uint32_t count;					// Must maps to al1_transfer_count_trig
};

int main()
{
    // Initialize stdio
    stdio_init_all();

    // Try to set a freq close to pixel clock (6172840 Hz) * 20 => 123456800 Hz.
    set_sys_clock_khz(125000, false);

    // Choose which PIO instance to use (there are two instances, each with 4 state machines)
    PIO pio = pio0;

   // pio program offsets for the cysnc and the rgb.
    const uint csync_offset = pio_add_program(pio, &csync_program);
    const uint rgb_offset = pio_add_program(pio, &rgb_program);

    // State machine for each program.
    const uint csync_sm = 0;
    const uint rgb_sm = 1;

    // Initialize each program.
    csync_program_init(pio, csync_sm, csync_offset, CSYNC_PIN);
    rgb_program_init(pio, rgb_sm, rgb_offset, RED_PIN);

    // Prepare the DMAs to do automatic data transfer.
    struct control_block_t control_blocks[] = {
        {0, &s_border_color, &pio->txf[rgb_sm], LINE_COUNT * BORDER_TOP_LINES},	   // top border
        {0, s_framebuffer, &pio->txf[rgb_sm], LINE_COUNT * RES_Y},				   // real pixels
        {0, &s_border_color, &pio->txf[rgb_sm], LINE_COUNT * BORDER_BOTTOM_LINES}, // botttom border
    };

    const uint channel_0 = dma_claim_unused_channel(true); // Transfer color
    const uint channel_1 = dma_claim_unused_channel(true); // Configure channel 1 to transfer top border + framebuffer + bottom border.
    const uint channel_2 = dma_claim_unused_channel(true); // Restart channel 2.

    {
        // Transfer colors to the PIO SM.
        dma_channel_config cfg = dma_channel_get_default_config(channel_0); // default configs
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);			// 8-bit txfers
        channel_config_set_read_increment(&cfg, true);						// yes read incrementing
        channel_config_set_write_increment(&cfg, false);					// no write incrementing
        channel_config_set_dreq(&cfg, pio_get_dreq(pio, rgb_sm, true));		// DREQ_PIO0_TX2 pacing (FIFO)
        channel_config_set_irq_quiet(&cfg, true);
        channel_config_set_chain_to(&cfg, channel_1);

        // ctrl for the pixels.
        control_blocks[1].ctrl = cfg.ctrl;

        // ctrl for the borders, the color is always the same so no read increment.
        channel_config_set_read_increment(&cfg, false);
        control_blocks[0].ctrl = cfg.ctrl;

        // for the last entry set the chain to channel 2 to trigger it once transfering finishes.
        channel_config_set_chain_to(&cfg, channel_2);
        control_blocks[2].ctrl = cfg.ctrl;
    }

    {
        // DMA channel 1 configure dma 0 (aka RGB data).
        dma_channel_config cfg = dma_channel_get_default_config(channel_1);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, true);
        channel_config_set_ring(&cfg, true, 4); // 16 byte boundary on write ptr
        channel_config_set_irq_quiet(&cfg, true);

        dma_channel_configure(channel_1,
                              &cfg,
                              &dma_hw->ch[channel_0].al1_ctrl, // Initial write address
                              control_blocks,				   // Initial read address
                              4,							   // Halt after each control block
                              false							   // Don't start yet
        );
    }

    const volatile void* control_block_ptr[] = {control_blocks};

    {
        // DMA Channel 2: restarts the DMA channel 1
        dma_channel_config cfg = dma_channel_get_default_config(channel_2); // default configs
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);			// 32-bit txfers
        channel_config_set_read_increment(&cfg, false);						// no read incrementing
        channel_config_set_write_increment(&cfg, false);					// no write incrementing
        channel_config_set_irq_quiet(&cfg, true);

        dma_channel_configure(channel_2,								 // Channel to be configured
                              &cfg,										 // The configuration we just created
                              &dma_hw->ch[channel_1].al3_read_addr_trig, // Write address (channel 1 read address)
                              control_block_ptr,						 // Read address (POINTER TO AN ADDRESS)
                              1,										 // Number of transfers, in this case each is 4 byte
                              false										 // Don't start immediately.
        );
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////////

    // Feed each state machine with the initial data.
    pio_sm_put_blocking(pio, csync_sm, SCAN_LINES - 1);
    pio_sm_put_blocking(pio, rgb_sm, (RES_X >> 1) - 2);

    // Enable the state machines.
    pio_enable_sm_mask_in_sync(pio, (1u << csync_sm) | (1u << rgb_sm));

    // Start DMA channel 1 to transfer the control blocks to dma which will send the RGB data.
    dma_start_channel_mask((1u << channel_1));

   // Feed the framebuffer with some vertical color bars.
    {
        uint color_index = 0;
        uint32_t vbar_length = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        for (y = 0; y < RES_Y; y++)
        {
            for (x = 0; x < RES_X; x++)
            {
                // vertical bar of 40 pixels
                if (vbar_length == 40)
                {
                    vbar_length = 0;
                    color_index = (color_index + 1) % 8;
                }
                vbar_length += 1;

                const uint8_t color = s_colors[color_index];
                const uint32_t offset = ((RES_X * y) + x);
                if (offset & 1)
                {
                    s_framebuffer[offset >> 1] |= (color << 3);
                }
                else
                {
                    s_framebuffer[offset >> 1] |= color;
                }
            }
        }
    }

    while (true)
    {
    }
}
