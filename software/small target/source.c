#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/sem.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/uart.h"
#include "ws2812.pio.h"

#define LED_PIN PICO_DEFAULT_LED_PIN
#define TEST_LED_0 4
#define TEST_LED_1 5

#define SS_CLK 14
#define SS_D 15
#define SS_LAT 16
#define SS_BLANK 17

#define LED_0 10

#define BAUD_RATE 31250 //MIDI standard data rate
#define UART0_TX 0
#define UART0_RX 1

#define PLAYER0_COLOR 0xFF000000
#define PLAYER1_COLOR 0x00FF0000
#define PLAYER2_COLOR 0x0000FF00
#define PLAYER3_COLOR 0x60900000

#define LEDS_PER_TARGET 35
#define TARGETS_PER_STRING 3
#define LED_COUNT (LEDS_PER_TARGET*TARGETS_PER_STRING)
#define TARGETS_TOTAL (TARGETS_PER_STRING*STRING_COUNT)
#define STRING_COUNT 2

#define UART_MSG_SIZE 3
#define BUFFER_CAPACITY 32

#define PIEZO1 20
#define PIEZO2 21

bool update_flag = false;
uint dma_chan[STRING_COUNT];
uint32_t led_colors[STRING_COUNT][LED_COUNT] = {0};
uint32_t current_pat[TARGETS_PER_STRING*STRING_COUNT] = {0};
uint32_t current_color[TARGETS_PER_STRING*STRING_COUNT]; //I think this should be eliminated
uint32_t current_player[TARGETS_PER_STRING*STRING_COUNT];
uint32_t current_time[TARGETS_PER_STRING*STRING_COUNT] = {0};
uint32_t preset_colors[16] = {PLAYER0_COLOR, PLAYER1_COLOR, PLAYER2_COLOR, PLAYER3_COLOR, 0, 0x88888800};
uint8_t uart_channal;
uint8_t program_numb =0;

volatile uint32_t time_click = 0;
uint32_t fade[35] = {
    0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 
    0, 0, 0, 0, 1, 
    2, 4, 8, 16, 32, 
    48, 64, 80, 96, 112,
    128, 144, 160, 176, 192,
    208, 224, 240, 256, 256
};

uint32_t blam[35] = {
    256, 256, 64, 8, 4, 
    2, 1, 0, 0, 0, 
    0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
};

bool timer_callback (struct repeating_timer *t) {
    time_click++;
    for (int i = 0; i < STRING_COUNT; i++)
    {
        if (dma_channel_is_busy(dma_chan[i])) {
            return false;
        }
    }
    
    for (int i = 0; i < STRING_COUNT; i++) {
        dma_channel_set_read_addr(dma_chan[i], led_colors[i], false );
        dma_channel_set_trans_count(dma_chan[i], LED_COUNT, false);
        dma_channel_start(dma_chan[i]);
    }
    update_flag = true;
    return true;
}

uint32_t set_brightness(u_int32_t color, uint32_t brightness){//Unity brightness is 256 colors is 0xGGRRBBWW
    u_int32_t green = ((color >> 24) & 0xFF) * brightness;
    u_int32_t red = ((color >> 16) & 0xFF) * brightness;
    u_int32_t blue = ((color >> 8) & 0xFF) * brightness;
    color =((green << 16) & 0xFF000000)+((red << 8) & 0x00FF0000)+((blue) & 0x0000FF00);
    return color;
}

void pattern_solid (uint32_t *data, uint32_t target_num){
    for (int i = 0; i < LEDS_PER_TARGET; i++)
    {
        //data[i]=current_color[target_num];
        data[i]=preset_colors[current_player[target_num]];
    }
}

void pattern_rotate (uint32_t *data, uint32_t target_num){
    for (int i = 0; i < LEDS_PER_TARGET; i++)
    {
        //data[i] = set_brightness(current_color[target_num], fade[(current_time[target_num] + LEDS_PER_TARGET - i)% LEDS_PER_TARGET]);
        data[i] = set_brightness(preset_colors[current_player[target_num]], fade[(current_time[target_num] + LEDS_PER_TARGET - i)% LEDS_PER_TARGET]);
    }
    (current_time[target_num] >= LEDS_PER_TARGET) ? current_time[target_num] = 0 : current_time[target_num]++;
}

void pattern_rotate_ccw (uint32_t *data, uint32_t target_num){
    for (int i = 0; i < LEDS_PER_TARGET; i++)
    {
        //data[i] = set_brightness(current_color[target_num], fade[(current_time[target_num]+i) % LEDS_PER_TARGET]);
        data[i] = set_brightness(preset_colors[current_player[target_num]], fade[(current_time[target_num]+i) % LEDS_PER_TARGET]);
    }
    (current_time[target_num] >= LEDS_PER_TARGET) ? current_time[target_num] = 0 : current_time[target_num]++;
}

void pattern_blamo (uint32_t *data, uint32_t target_num){ //needs more blam
    for (int i = 0; i < LEDS_PER_TARGET; i++)
    {
        //data[i] = set_brightness(current_color[target_num], blam[current_time[target_num]]);
        data[i] = set_brightness(preset_colors[current_player[target_num]], blam[current_time[target_num]]);   
    }
    (current_time[target_num] >= LEDS_PER_TARGET) ? current_pat[target_num] = 0 : current_time[target_num]++;
}

void pattern_off (uint32_t *data, uint32_t target_num){
    for (int i = 0; i < LEDS_PER_TARGET; i++)
    {
        data[i]= 0;
        current_time[target_num] = 0;
    }
}
typedef void (*pattern)(uint32_t *data, uint32_t target_num);
const struct {
    pattern pat;
    const char *name;
} pattern_table[] = {
        {pattern_off,  "Off"},
        {pattern_blamo,  "Blamo!"},
        {pattern_solid,  "Solid"},
        {pattern_rotate,  "Rotate"},
        {pattern_rotate_ccw,  "Counter Rotate"},
        
        
};


void setup_dma(PIO pio, uint sm, uint dma_chan, const uint32_t *source_addr) {
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(dma_chan, &c, &pio->txf[sm], source_addr, LED_COUNT, false);
}

void setup_leds() {
    for (int i = 0; i < STRING_COUNT; i++) {
        for (int j = 0; j < LED_COUNT; j++) {
            led_colors[i][j] = 0x000000; // RBG Color
        }
    }
}

uint8_t ring_buffer[BUFFER_CAPACITY][UART_MSG_SIZE];
volatile int write_index = 0;
volatile int read_index = 0;
volatile int buffer_count = 0;

void target_hit(uint8_t channel, uint8_t note, u_int8_t player, u_int8_t score){ //Player 3 bits, score 5!
            uart_putc(uart0, 0x90 | channel); //Must be 0 - 15
            uart_putc(uart0, note); //Must be 0 - 127
            uart_putc(uart0, ((player<<5)|(score&0x1F))); //Must be 0 - 127
            printf("Target Hit: 0x%02X 0x%02X 0x%02X -score 0x%02X\n", (0x90 | channel), note, ((player<<5)|(score&0x1F)), score);
}

// Function to handle UART interrupts. Currently hardcoded to uart0!
void on_uart_rx() {
    static uint8_t uart_data_buffer[UART_MSG_SIZE];
    static int uart_data_index = 0;

    while (uart_is_readable(uart0)) {
        uint8_t byte = uart_getc(uart0);
        printf("RX byte: 0x%02X index %d\n", byte, uart_data_index);

        // MIDI messages start with a status byte (0x80 to 0xFF)
        if (byte & 0x80) {
            uart_data_index = 0;  // Reset buffer index if we get a new status byte
        }

        uart_data_buffer[uart_data_index++] = byte;

        // Check if we've received a complete MIDI message
        if (uart_data_index == UART_MSG_SIZE) {
            // Validate that the first byte is a status byte
            if (uart_data_buffer[0] & 0x80) {
                // Store the message in the ring buffer if there is space
                if (buffer_count < BUFFER_CAPACITY) {
                    for (int i = 0; i < UART_MSG_SIZE; i++) {
                        ring_buffer[write_index][i] = uart_data_buffer[i];
                    }
                    write_index = (write_index + 1) % BUFFER_CAPACITY;
                    buffer_count++;
                }
            }
            uart_data_index = 0;  // Reset buffer index for next message
        }
    }
}

void process_uart_data(uint8_t *data) {
    uint8_t command = data[0]>>4;
    uint8_t data1 = data[1];
    uint8_t data2 = data[2];
    //printf("MIDI Message: 0x%02X 0x%02X 0x%02X\n", data[0], data1, data2);
    switch (command)
    {
    case 0x08://Note off
        //current_color[data1] = 0;
        current_pat[data1] = 0;
        current_time[data1] = 0;
        break;
    case 0x09://Note on
        //current_color[data1] = preset_colors[data2>>4];
        current_player[data1] = data2>>4;
        current_pat[data1] = data2&0x0F;
        printf("Target on: 0x%02X note 0x%02X player 0x%08X pat 0x%02X\n", command, data1, current_player[data1], current_pat[data1]);
        break;
    case 0x0A://Polyphonic Aftertouch
        /* code */
        break;
    case 0x0B://Control Change
        /* need to forward target num and respond with num of targets */
        uart_putc(uart0, data[0]); //Must be 0 - 15
        uart_putc(uart0, 0); //Must be 0 - 127
        uart_putc(uart0, data[2]+TARGETS_TOTAL); //Must be 0 - 127
        printf("Number of targets: %d\n", data[2]+TARGETS_TOTAL);
        break;
    case 0x0C://Program Change
        program_numb = data1;
        break;
    case 0x0D://Channel Aftertouch
        /* code */
        break;
    case 0x0E://Pitch Wheel
        /* code */
        break;
    default:
        break;
    }
}

void gpio_callback(uint gpio, uint32_t events){
    if(gpio == PIEZO1){
        if(current_pat[0]>1){
            target_hit(0,0,current_player[0],current_time[0]);
            printf("Target - Player Hit: %d\n", current_player[0]);
            current_time[0] = 0;
            current_pat[0] = 1;
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

    gpio_init(PIEZO1);
    gpio_set_dir(PIEZO1, false);
    gpio_init(PIEZO2);
    gpio_set_dir(PIEZO2, false);
    gpio_set_irq_enabled_with_callback(PIEZO1, GPIO_IRQ_EDGE_RISE, true, &gpio_callback);

    //Initalize UARTs
    uart_init(uart0, BAUD_RATE);
    gpio_set_function(UART0_TX, GPIO_FUNC_UART);
    gpio_set_function(UART0_RX, GPIO_FUNC_UART);

    //Enable UART interrupt
    uart_set_irq_enables(uart0, true, false);
    irq_set_exclusive_handler(UART0_IRQ, on_uart_rx);
    irq_set_enabled(UART0_IRQ, true);

    // Initialize hardware timer
    struct repeating_timer timer;

    PIO pio = pio0;
    uint offset = pio_add_program(pio, &ws2812_program);
    //setup_leds();

    for (int i = 0; i < STRING_COUNT; i++) {
        uint sm = pio_claim_unused_sm(pio, true);
        ws2812_program_init(pio, sm, offset, i + LED_0, 800000, false); // Assuming GPIO pins 0-3 for the 4 strips
        dma_chan[i] = dma_claim_unused_channel(true);
        setup_dma(pio, sm, dma_chan[i], led_colors[i]);
        dma_channel_start(dma_chan[i]);
    }
    // Initialize the timer with the given period and enable interrupts
    add_repeating_timer_ms(-20, timer_callback, NULL, &timer); //-20 for 50 fps  

    while(true){
        gpio_put(TEST_LED_0, gpio_get(PIEZO1));
        gpio_put(TEST_LED_1, gpio_get(PIEZO2));
        gpio_put(LED_PIN, 1);
        if (buffer_count > 0) {
            process_uart_data(ring_buffer[read_index]);
            read_index = (read_index + 1) % BUFFER_CAPACITY;
            buffer_count--;
        }
        if (update_flag) {
            for (int i = 0; i < TARGETS_TOTAL; i++) {
                pattern_table[current_pat[i]].pat(&led_colors[i / TARGETS_PER_STRING][(i % TARGETS_PER_STRING) * LEDS_PER_TARGET], i);
            }
        update_flag = false;
        //printf("update flag 0!\n");
        }
        //printf("Main loop!\n");
    }
}