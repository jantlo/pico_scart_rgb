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

#define RES_X 320
#define RES_Y 304

// Length of the pixel array, and number of DMA transfers
#define TXCOUNT ((RES_X >> 1) * RES_Y) // Total pixels/2 (since we have 2 pixels per byte)

// Pixel color array that is DMA's to the PIO machines and
// a pointer to the ADDRESS of this color array.
// Note that this array is automatically initialized to all 0's (black)
static unsigned char s_framebuffer[TXCOUNT];
static const unsigned char* s_address_pointer[1] = {s_framebuffer};
// static unsigned char* address_pointer = &framebuffer[0];

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

// A function for drawing a pixel with a specified color.
// Note that because information is passed to the PIO state machines through
// a DMA channel, we only need to modify the contents of the array and the
// pixels will be automatically updated on the screen.
void drawPixel(int x, int y, char color)
{
    const int max_x = RES_X - 1;
    const int max_y = RES_Y - 1;
    // Range checks
    if (x > max_x)
        x = max_x;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (y > RES_Y)
        y = RES_Y;

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
        s_framebuffer[pixel >> 1] |= (color);
    }
}

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

    // DMA channels - 0 sends color data, 1 reconfigures and restarts 0
    const uint rgb_dma_chan_0 = dma_claim_unused_channel(true);
    const uint rgb_dma_chan_1 = dma_claim_unused_channel(true);

    // Channel Zero (sends color data to PIO RGB machine)
    dma_channel_config dma_cfg_0 = dma_channel_get_default_config(rgb_dma_chan_0); // default configs
    channel_config_set_transfer_data_size(&dma_cfg_0, DMA_SIZE_8);				   // 8-bit txfers
    channel_config_set_read_increment(&dma_cfg_0, true);						   // yes read incrementing
    channel_config_set_write_increment(&dma_cfg_0, false);						   // no write incrementing
    channel_config_set_dreq(&dma_cfg_0, pio_get_dreq(pio, rgb_sm, true));		   // DREQ_PIO0_TX2 pacing (FIFO)
    channel_config_set_irq_quiet(&dma_cfg_0, true);
    channel_config_set_chain_to(&dma_cfg_0, rgb_dma_chan_1); // chain to other channel

    dma_channel_configure(rgb_dma_chan_0,	 // Channel to be configured
                          &dma_cfg_0,		 // The configuration we just created
                          &pio->txf[rgb_sm], // write address (RGB PIO TX FIFO)
                          &s_framebuffer,		 // The initial read address (pixel color array)
                          TXCOUNT,			 // Number of transfers; in this case each is 1 byte.
                          false				 // Don't start immediately.
    );

    // Channel One (reconfigures the first channel)
    dma_channel_config dma_cfg_1 = dma_channel_get_default_config(rgb_dma_chan_1); // default configs
    channel_config_set_transfer_data_size(&dma_cfg_1, DMA_SIZE_32);				   // 32-bit txfers
    channel_config_set_read_increment(&dma_cfg_1, false);						   // no read incrementing
    channel_config_set_write_increment(&dma_cfg_1, false);						   // no write incrementing
    channel_config_set_irq_quiet(&dma_cfg_1, true);
    channel_config_set_chain_to(&dma_cfg_1, rgb_dma_chan_0); // chain to other channel

    dma_channel_configure(rgb_dma_chan_1,						 // Channel to be configured
                          &dma_cfg_1,							 // The configuration we just created
                          &dma_hw->ch[rgb_dma_chan_0].read_addr, // Write address (channel 0 read address)
                          s_address_pointer,						 // Read address (POINTER TO AN ADDRESS)
                          1,									 // Number of transfers, in this case each is 4 byte
                          false									 // Don't start immediately.
    );

    /////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////////

    // Initialize PIO state machine counters. This passes the information to the state machines
    // that they retrieve in the first 'pull' instructions, before the .wrap_target directive
    // in the assembly. Each uses these values to initialize some counting registers.
    pio_sm_put_blocking(pio, csync_sm, RES_Y - 1);
    pio_sm_put_blocking(pio, rgb_sm, (RES_X >> 1) - 2);

    // Start the two pio machine IN SYNC
    // Note that the RGB state machine is running at full speed,
    // so synchronization doesn't matter for that one. But, we'll
    // start them all simultaneously anyway.
    pio_enable_sm_mask_in_sync(pio, (1u << csync_sm) | (1u << rgb_sm));

    // Start DMA channel 0. Once started, the contents of the pixel color array
    // will be continously DMA's to the PIO machines that are driving the screen.
    // To change the contents of the screen, we need only change the contents
    // of that array.
    dma_start_channel_mask((1u << rgb_dma_chan_1));

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

            if (ycounter == 60)
            {							 //   If the y-counter is 60 . . .
                ycounter = 0;			 //     Zero the counter
                index = (index + 1) % 8; //     Increment the color index
            }							 //
            ycounter += 1;				 //   Increment the y-counter
            for (x = 0; x < RES_X; x++)
            { //   For each x-coordinate . . .
                if (y > 290)
                {
                    drawPixel(x, y, RED);
                }
                else
                {
                    if (xcounter == 40)
                    {								//     If the x-counter is 80 . . .
                        xcounter = 0;				//        Zero the x-counter
                        index = (index + 1) % 8;	//        Increment the color index
                    }								//
                    xcounter += 1;					//     Increment the x-counter
                    drawPixel(x, y, colors[index]); //     Draw a pixel to the screen
                }
            }
        }
    }

    while (true)
    {
    }
}

