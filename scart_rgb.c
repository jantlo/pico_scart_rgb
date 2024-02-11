/**
 * Hunter Adams (vha3@cornell.edu)
 *
 * VGA driver using PIO assembler
 *
 * HARDWARE CONNECTIONS
 *  - GPIO 16 ---> csync
 *  - GPIO 18 ---> 330 ohm resistor ---> VGA Red
 *  - GPIO 19 ---> 330 ohm resistor ---> VGA Green
 *  - GPIO 20 ---> 330 ohm resistor ---> VGA Blue
 *  - RP2040 GND ---> VGA GND
 *
 * RESOURCES USED
 *  - PIO state machines 0, 1, and 2 on PIO instance 0
 *  - DMA channels 0 and 1
 *  - 153.6 kBytes of RAM (for pixel color data)
 *
 * HOW TO USE THIS CODE
 *  This code uses one DMA channel to send pixel data to a PIO state machine
 *  that is driving the VGA display, and a second DMA channel to reconfigure
 *  and restart the first. As such, changing any value in the pixel color
 *  array will be automatically reflected on the VGA display screen.
 *
 *  To help with this, I have included a function called drawPixel which takes,
 *  as arguments, a VGA x-coordinate (int), a VGA y-coordinate (int), and a
 *  pixel color (char). Only 3 bits are used for RGB, so there are only 8 possible
 *  colors. If you keep all of the code above line 200, this interface will work.
 *
 */
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include <stdio.h>
// Our assembled programs:
// Each gets the name <pio_filename.pio.h>
#include "csync.pio.h"
#include "rgb.pio.h"

#define SCAN_LINES 304
#define BORDER_TOP_LINES 42
#define BORDER_BOTTOM_LINES 22

#define RES_X 320
#define RES_Y (SCAN_LINES - BORDER_TOP_LINES - BORDER_BOTTOM_LINES)

// Length of the pixel array, and number of DMA transfers
#define LINE_COUNT (RES_X >> 1)
#define FRAMEBUFFER_SIZE (LINE_COUNT * RES_Y) // Total pixels/2 (since we have 2 pixels per byte)

// Give the I/O pins that we're using some names that make sense
#define CSYNC 16
#define RED_PIN 18
#define GREEN_PIN 19
#define BLUE_PIN 20

// We can only produce 8 colors, so let's give them readable names
#define BLACK 0
#define RED 1
#define GREEN 2
#define YELLOW 3
#define BLUE 4
#define MAGENTA 5
#define CYAN 6
#define WHITE 7

// Pixel color array that is DMA's to the PIO machines and
// a pointer to the ADDRESS of this color array.
// Note that this array is automatically initialized to all 0's (black)
static uint8_t s_framebuffer[FRAMEBUFFER_SIZE];
static const uint8_t* s_address_pointer[1] = {s_framebuffer};
static const uint8_t s_border_color = 0; //(GREEN << 3) | GREEN;

// A function for drawing a pixel with a specified color.
// Note that because information is passed to the PIO state machines through
// a DMA channel, we only need to modify the contents of the array and the
// pixels will be automatically updated on the screen.
void draw_pixel(uint32_t x, uint32_t y, uint8_t color)
{
    const uint32_t k_max_x = RES_X - 1;
    const uint32_t k_max_y = RES_Y - 1;
    // Range checks
    if (x > k_max_x)
        x = k_max_x;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (y > k_max_y)
        y = k_max_y;

    // Which pixel is it?
    int pixel = ((RES_X * y) + x);

    // Is this pixel stored in the first 3 bits
    // of the vga data array index, or the second
    // 3 bits? Check, then mask.
    if (pixel & 1)
    {
        s_framebuffer[pixel >> 1] |= (color << 3);
    }
    else
    {
        s_framebuffer[pixel >> 1] |= color;
    }
}

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

    // Our assembled program needs to be loaded into this PIO'sset pins, 1 [28] instruction
    // memory. This SDK function will find a location (offset) in the
    // instruction memory where there is enough space for our program. We need
    // to remember these locations!
    //
    // We only have 32 instructions to spend! If the PIO programs contain more than
    // 32 instructions, then an error message will get thrown at these lines of code.
    //
    // The program name comes from the .program part of the pio file
    // and is of the form <program name_program>
    const uint csync_offset = pio_add_program(pio, &csync_program);
    const uint rgb_offset = pio_add_program(pio, &rgb_program);

    // Manually select a few state machines from pio instance pio0.
    const uint csync_sm = 0;
    const uint rgb_sm = 1;

    // Call the initialization functions that are defined within each PIO file.
    // Why not create these programs here? By putting the initialization function in
    // the pio file, then all information about how to use/setup that state machine
    // is consolidated in one place. Here in the C, we then just import and use it.
    csync_program_init(pio, csync_sm, csync_offset, CSYNC);
    rgb_program_init(pio, rgb_sm, rgb_offset, RED_PIN);

    /////////////////////////////////////////////////////////////////////////////////////////////////////
    // ===========================-== DMA Data Channels =================================================
    /////////////////////////////////////////////////////////////////////////////////////////////////////

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

    // Initialize PIO state machine counters. This passes the information to the state machines
    // that they retrieve in the first 'pull' instructions, before the .wrap_target directive
    // in the assembly. Each uses these values to initialize some counting registers.
    pio_sm_put_blocking(pio, csync_sm, SCAN_LINES - 1);
    pio_sm_put_blocking(pio, rgb_sm, (RES_X >> 1) - 2);

    // Start the two pio machine IN SYNC
    // Note that the RGB state machine is running at full speed,
    // so synchronization doesn't matter for that one. But, we'll
    // start them all simultaneously anyway.
    pio_enable_sm_mask_in_sync(pio, (1u << csync_sm) | (1u << rgb_sm));

    // Start DMA channel 1 to transfer the control blocks to dma which will send the RGB data.
    dma_start_channel_mask((1u << channel_1));

    /////////////////////////////////////////////////////////////////////////////////////////////////////
    // ===================================== An Example =================================================
    /////////////////////////////////////////////////////////////////////////////////////////////////////
    //
    // The remainder of this program is simply an example to show how to use the VGA system.
    // This particular example just produces a diagonal array of colors on the VGA screen.
    {
        // Array of colors, and a variable that we'll use to index into the array
        static const char colors[8] = {BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE};
        uint index = 0;

        // A couple of counters
        uint xcounter = 0;
        uint ycounter = 0;

        uint x = 0;
        uint y = 0;
        for (y = 0; y < RES_Y; y++)
        { // For each y-coordinate . . .

            if (ycounter == 40)
            {							 //   If the y-counter is 60 . . .
                ycounter = 0;			 //     Zero the counter
                index = (index + 1) % 8; //     Increment the color index
            }							 //
            ycounter += 1;				 //   Increment the y-counter
            for (x = 0; x < RES_X; x++)
            { //   For each x-coordinate . . .
                if (xcounter == 40)
                {								 //     If the x-counter is 80 . . .
                    xcounter = 0;				 //        Zero the x-counter
                    index = (index + 1) % 8;	 //        Increment the color index
                }								 //
                xcounter += 1;					 //     Increment the x-counter
                draw_pixel(x, y, colors[index]); //     Draw a pixel to the screen
            }
        }
    }

    while (true)
    {
    }
}
