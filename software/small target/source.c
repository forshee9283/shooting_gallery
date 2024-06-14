#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/sem.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "ws2812.pio.h"

#define LED_PIN PICO_DEFAULT_LED_PIN
#define TEST_LED_0 4
#define TEST_LED_1 5

#define SS_CLK 14
#define SS_D 15
#define SS_LAT 16
#define SS_BLANK 17

#define LED_0 10

#define UART0_TX 0
#define UART0_RX 1

#define LED_COUNT 70
#define STRIP_COUNT 2

bool update_flag = false;
uint dma_chan[STRIP_COUNT];
uint32_t led_colors[STRIP_COUNT][LED_COUNT];

bool timer_callback (struct repeating_timer *t) {
    for (int i = 0; i < STRIP_COUNT; i++)
    {
        if (dma_channel_is_busy(dma_chan[i])) {
            return false;
        }
    }
    
    for (int i = 0; i < STRIP_COUNT; i++) {
        dma_channel_set_read_addr(dma_chan[i], led_colors[i], false );
        dma_channel_set_trans_count(dma_chan[i], LED_COUNT, false);
        dma_channel_start(dma_chan[i]);
    }
    update_flag = true;
    return true;
}

void setup_dma(PIO pio, uint sm, uint dma_chan, const uint32_t *source_addr) {
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(dma_chan, &c, &pio->txf[sm], source_addr, LED_COUNT, false);
}

void setup_leds() {
    for (int i = 0; i < STRIP_COUNT; i++) {
        for (int j = 0; j < LED_COUNT; j++) {
            led_colors[i][j] = 0x0F0000; // RBG Color
        }
    }
}

int main() {
    stdio_init_all();
    gpio_init(TEST_LED_0);
    gpio_set_dir(TEST_LED_0, true);
    gpio_init(TEST_LED_1);
    gpio_set_dir(TEST_LED_1, true);
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, true);

    // Initialize hardware timer
    struct repeating_timer timer;

    PIO pio = pio0;
    uint offset = pio_add_program(pio, &ws2812_program);
    setup_leds();

    for (int i = 0; i < STRIP_COUNT; i++) {
        uint sm = pio_claim_unused_sm(pio, true);
        ws2812_program_init(pio, sm, offset, i + LED_0, 800000, false); // Assuming GPIO pins 0-3 for the 4 strips
        dma_chan[i] = dma_claim_unused_channel(true);
        setup_dma(pio, sm, dma_chan[i], led_colors[i]);
        dma_channel_start(dma_chan[i]);
    }
    sleep_ms(500);
    // Initialize the timer with the given period and enable interrupts
    add_repeating_timer_ms(-20, timer_callback, NULL, &timer); //50 fps

    while(true){
        gpio_put(TEST_LED_0, 1);
        gpio_put(TEST_LED_1, update_flag);
        gpio_put(LED_PIN, 1);
        if (update_flag) {
            // for (int i = 0; i < STRIP_COUNT; i++) {
            //     for (int j = 0; j < LED_COUNT; j++) {
            //         led_colors[i][j] = 0x03030300; // RGB Color
            //     }
            // }
            for (int j = 0; j < LED_COUNT; j++) {
                led_colors[0][j] = 0x03030000; // RGB Color
            }
            for (int j = 0; j < LED_COUNT; j++) {
                //led_colors[1][j] = 0x00030300; // RGB Color
                led_colors[1][j] = (j)<<8; // RGB Color
            }
            led_colors[1][0] = 0x000F0000; //First LED RED
            led_colors[1][LED_COUNT-1] = 0x0F000000; //Last LED green
        update_flag = false;
        }
        printf("I AM RUNNING!! \n");
        sleep_ms(200);
    }
}