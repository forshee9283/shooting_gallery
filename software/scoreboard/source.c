
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/sem.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "ascii_to_seg.h"

#define LED_PIN PICO_DEFAULT_LED_PIN
#define TEST_LED_0 2
#define SW_1 28
#define SW_2 27
#define SW_3 26
#define DEBOUNCE_DELAY_MS 5

#define SS_SPI spi0
#define SS_SPI_RATE 1000000 //1MHz
#define SS_CLK 6
#define SS_D 7
#define SS_LAT 8
#define SS_BLANK 9

#define BAUD_RATE 31250 //MIDI standard data rate
#define UART0_TX 0
#define UART0_RX 1
#define UART1_TX 4
#define UART1_RX 5
#define UART_MSG_SIZE 3
#define BUFFER_CAPACITY 32

#define MAX_PLAYERS 4 //Set for number of player scoreboards in the chain
#define DISPLAY_UNITS MAX_PLAYERS + 1 //Total number of displays with clock
volatile int score[MAX_PLAYERS] = {0};

volatile bool sw_flag[3] = {0};
volatile int led_on_timer = 0;
volatile int piezo_trig_cnt = 0;
volatile uint game_time = 0;
volatile uint num_targets;
volatile uint game_mode = 0;
volatile uint next_mode = 1;
const char *modes_lables[] = {"SETUP  ", "- lights out -", "run 3  "};
#define num_modes 3
volatile uint timer_tic = 0;

uint current_players = MAX_PLAYERS;

uint8_t segments[10] = {
    0b00111111, // 0
    0b00000110, // 1
    0b01011011, // 2
    0b01001111, // 3
    0b01100110, // 4
    0b01101101, // 5
    0b01111101, // 6
    0b00000111, // 7
    0b01111111, // 8
    0b01101111  // 9
    };
uint8_t boot_message[24] = {
    0b00111001, //C
    0b00111111, //O
    0b00111111, //O
    0b01110011, //P
    0b01111001, //E
    0b01010000, //r
    0b00000010, //'
    0b01101101, //S
    0b00000000, //space
    0b01101101, //S
    0b00111110, //U
    0b01110011, //P
    0b01111001, //E
    0b01010000, //r
    0b00000000, //space
    0b01111100, //b
    0b00111110, //U
    0b00111000, //L
    0b00111000, //L
    0b01101101, //S
    0b01111001, //E
    0b01101110, //y
    0b01111001, //E
    0b00000000  //space
    };
char start_message[] = "COOPEr'S SUPER bULLSEyE ";  

absolute_time_t last_pressed_time[3] = {0};

bool is_debounce(uint sw_index) { //Hardware debounce was nessisary in testing
    absolute_time_t now = get_absolute_time();
    int64_t time_diff = absolute_time_diff_us(last_pressed_time[sw_index], now);
    if (time_diff < (DEBOUNCE_DELAY_MS * 1000)) {
        return true;  // Ignore this event
    }    
    last_pressed_time[sw_index] = now; // Update the last press time
    return false;  // Process this event
}

void ss_int_write(int num){
    uint8_t data_to_send [4];
    data_to_send [3] = segments[num % 10];
    data_to_send [2] = segments[(num /10) % 10];
    data_to_send [1] = segments[(num /100) % 10];
    data_to_send [0] = segments[(num /1000) % 10];
    spi_write_blocking(SS_SPI, &data_to_send[0], 4);
}

void ss_int_blank_write(int num){
    uint8_t data_to_send [4] = {0};
    if(num){
        data_to_send [3] = segments[num % 10];
    }
    if(num>=10){
        data_to_send [2] = segments[(num /10) % 10];
    }
    if(num>=10){
        data_to_send [1] = segments[(num /100) % 10];
    }
    if(num>=10){
        data_to_send [0] = segments[(num /1000) % 10];
    }
    spi_write_blocking(SS_SPI, &data_to_send[0], 4);
}

void ss_time_write(int num){
    uint8_t data_to_send [4];
    uint8_t blink = ((num % 10)>4) ? 0x00 : 0x80; //blink ":" between min and sec
    int seconds = (num/10)%60;
    data_to_send [3] = segments[num % 10];
    data_to_send [2] = segments[(seconds) % 10] | 0x80;
    data_to_send [1] = segments[(seconds /10) % 10]| blink;
    data_to_send [0] = segments[(num /600) % 10] | blink;
    spi_write_blocking(SS_SPI, &data_to_send[0], 4);
}

void ss_string_write(const char* input_str, int shift) {
    uint8_t data_to_send[4];
    int length = strlen(input_str);

    // If the string is 4 characters or less, no shift needed
    if (length <= 4) {
        // Copy the string directly into the display buffer
        for (int i = 0; i < 4; i++) {
            if (i < length) {
                data_to_send[i] = ASCII_TO_SEG[input_str[i] - 32];  // Convert character
            } else {
                data_to_send[i] = 0;  // Pad with 0 if the string is shorter than 4 chars
            }
        }
    } else {
        // Handle scrolling if the string is longer than 4 characters
        for (int i = 0; i < 4; i++) {
            int index = (shift + i) % length;  // Wrap around when reaching the end of the string
            data_to_send[i] = ASCII_TO_SEG[input_str[index] - 32];  // Convert character
        }
    }

    // Send the data to the display
    spi_write_blocking(SS_SPI, &data_to_send[0], 4);
}

bool timer_callback (struct repeating_timer *t) {
    timer_tic++;
    int shift = timer_tic>>2;
    gpio_put(SS_LAT, 0);
    switch (game_mode){
    case 0: //setup
        ss_string_write(start_message, shift);
        ss_string_write("PLyr", 0);
        ss_int_blank_write(current_players);
        ss_string_write("TyPE", 0);
        ss_string_write(modes_lables[next_mode], shift);
        break;
    case 1:
        if (game_time != 0){
            game_time--;
        }
        ss_time_write(game_time);
        for (size_t i = 0; i < MAX_PLAYERS; i++){
            if(i<current_players){
                ss_int_write(score[i]);
            }
            else{
                ss_int_blank_write(0);
            }
        }
        break;
    default:
        break;
    }
    gpio_put(SS_LAT, 1);
    return true;
}



void set_tlc5916_current(uint8_t* currents, int groups) {
    int total_chips = groups * 4;

    int blank_start_special [5] = {1,0,1,1,1};
    int latch_start_special [5] = {0,0,0,1,0};
    int blank_end_special [5] = {1,0,1,1,1};
    int latch_end_special [5] = {0,0,0,0,0};
    
    // Enter special mode
    spi_deinit(SS_SPI);
    gpio_set_function(SS_D, GPIO_FUNC_SIO);
    gpio_set_dir(SS_D, true);
    gpio_set_function(SS_CLK, GPIO_FUNC_SIO);
    gpio_set_dir(SS_CLK, true);
    for (int i = 0; i < 4; i++) {
    gpio_put(SS_BLANK, blank_start_special[i]);
    gpio_put(SS_LAT, latch_start_special[i]);
    gpio_put(SS_CLK,1);
    sleep_us(100);
    gpio_put(SS_CLK,0);
    sleep_us(100);
    }

    // // Set the output current for each group
    for (int g = 0; g < groups; g++) {
        uint8_t current = currents[g];
        for (int i = 0; i < 4; i++) {
            for (int bit = 0; bit < 8; bit++) {
                uint8_t current_bit = (current >> (7 - bit)) & 0x01; // Extract the bit
                gpio_put(SS_LAT,(bit==7)?1:0);
                gpio_put(SS_D, current_bit); // Send the bit
                gpio_put(SS_CLK,1);
                sleep_us(100);
                gpio_put(SS_CLK,0);
                sleep_us(100);
            }
        }
    }

    // Return to normal mode
    for (int i = 0; i < 4; i++) {
    gpio_put(SS_BLANK, blank_end_special[i]);
    gpio_put(SS_LAT, latch_end_special[i]);
    gpio_put(SS_CLK,1);
    sleep_us(100);
    gpio_put(SS_CLK,0);
    sleep_us(100);
    }
    spi_init(SS_SPI, SS_SPI_RATE); // Initialize SPI
    gpio_set_function(SS_D, GPIO_FUNC_SPI);
    gpio_set_function(SS_CLK, GPIO_FUNC_SPI);
    gpio_put(SS_BLANK, 0);
}
//Currently hardcoded for 4 displays
void boot_text() {
    
    for (size_t i = 0; i < 100; i++)
    {
        gpio_put(SS_LAT, 0);
        for (size_t j = 0; j < 12; j++) //This will need redoing with all the displays
        {
            spi_write_blocking(SS_SPI, &boot_message[((i + j) % sizeof(boot_message))], 1);
        }
        gpio_put(SS_LAT, 1);
        sleep_ms(400);
    }
    

}

//Com Functions - These are based on midi
void target_init(uint8_t channel){
            uart_putc(uart0, 0xB0 | channel); //Must be 0 - 15
            uart_putc(uart0, 0); //Must be 0 - 127
            uart_putc(uart0, 0); //Must be 0 - 127
}
void target_on(uint8_t channel, uint8_t note, u_int8_t color, u_int8_t pattern){
            uart_putc(uart0, 0x90 | channel); //Must be 0 - 15
            uart_putc(uart0, note); //Must be 0 - 127
            uart_putc(uart0, ((color<<4)|pattern)); //Must be 0 - 127
            printf("TX Note on Message: 0x%02X 0x%02X 0x%02X\n", (0x90 | channel), note, ((color<<4)|pattern));
}
void target_off(uint8_t channel, uint8_t note, u_int8_t data){
            uart_putc(uart0, 0x80 | channel); //Must be 0 - 15
            uart_putc(uart0, note); //Must be 0 - 127
            uart_putc(uart0, data); //Must be 0 - 127
}

uint8_t ring_buffer[BUFFER_CAPACITY][UART_MSG_SIZE];
volatile int write_index = 0;
volatile int read_index = 0;
volatile int buffer_count = 0;

// Function to handle UART interrupts. Currently hardcoded to uart0!
void on_uart_rx() {
    static uint8_t uart_data_buffer[UART_MSG_SIZE];
    static int uart_data_index = 0;

    while (uart_is_readable(uart0)) {
        uint8_t byte = uart_getc(uart0);
        //printf("Scoreboard RX byte: 0x%02X index %d\n", byte, uart_data_index);
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
        break;
    case 0x09://Note on Hit Recived
        // current_color[data1] = preset_colors[data2>>4];
        // current_pat[data1] = data2&0x0F;
        printf("RX on: Channel:%d Target:%d Player:%d Points:%d data2:0x%02X\n", data[0]&0xF, data1, data2>>5, data2&0x1F, data2);
        break;
    case 0x0A://Polyphonic Aftertouch
        /* code */
        break;
    case 0x0B://Control Change
        num_targets = num_targets>data[2] ? num_targets : data[2]; 
        printf("Scoreboard - Number of targets: %d\n", num_targets);
        break;
    case 0x0C://Program Change
        /* code */
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

void target_enum() { //hardcoded for uart0
    uart_putc(uart0, 0xB0);
    uart_putc(uart0, 0); 
    uart_putc(uart0, 0); 
    sleep_ms(300);//wait for responce
}

void setup_mode(){
    if(sw_flag[0]){
        game_time = 1800;
        current_players = (current_players % MAX_PLAYERS) +1;
        sw_flag[0] = 0;
        }
    if(sw_flag[1]){
        game_time = 1800; //probably should make an array for times.
        if(next_mode == 0){
            target_enum();
        }
        //Add light show to indicate start here
        for (size_t i = 0; i < num_targets; i++)
        {
            //target_on(1, i, rand()%4, (rand()%4)+2);
            target_on(1, i, rand()%4, 5);
            sleep_ms(500);
        }
        sleep_ms(1750);
        //printf("Button Press\n");
        game_mode = next_mode;
        sw_flag[1] = 0;
    }
    if(sw_flag[2]){
        next_mode = (next_mode + 1) % num_modes;
        sw_flag[2] = 0;
    }
    score[0]++;
    score[1]+=3;
    score[2]+=5;
    score[3]=num_targets;
    //sleep_ms(300);
}

void timer_mode(){
    if(game_time == 0){
        sleep_ms(2000);
        game_mode = 0;
    }
}

void lights_out_mode(){
    game_mode = 0;
}

void gpio_callback(uint gpio, uint32_t events){
    if (gpio == SW_1 && !is_debounce(0)) {
        sw_flag[0] = 1;
    }
    if (gpio == SW_2 && !is_debounce(1)) {
        sw_flag[1] = 1;
    }
    if (gpio == SW_3 && !is_debounce(2)) {
        sw_flag[2] = 1;
    }
}

int main() {
    stdio_init_all();
    gpio_init(TEST_LED_0);
    gpio_set_dir(TEST_LED_0, true);
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, true);

    //Button inputs
    gpio_init(SW_1);
    gpio_pull_up(SW_1);
    gpio_set_dir(SW_1, false);
    gpio_init(SW_2);
    gpio_pull_up(SW_2);
    gpio_set_dir(SW_2, false);
    gpio_init(SW_3);
    gpio_pull_up(SW_3);
    gpio_set_dir(SW_3, false);

    //Button interupt setup
    gpio_set_irq_enabled_with_callback(SW_1, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
    gpio_set_irq_enabled_with_callback(SW_2, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
    gpio_set_irq_enabled_with_callback(SW_3, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);

    // Initialize RS232
    uart_init(uart0, BAUD_RATE);
    gpio_set_function(UART0_TX, GPIO_FUNC_UART);
    gpio_set_function(UART0_RX, GPIO_FUNC_UART);

    //Enable UART interrupt
    uart_set_irq_enables(uart0, true, false);
    irq_set_exclusive_handler(UART0_IRQ, on_uart_rx);
    irq_set_enabled(UART0_IRQ, true);   

    //Seven segmet outputs
    gpio_init(SS_LAT);
    gpio_set_dir(SS_LAT, true);
    gpio_init(SS_BLANK);
    gpio_set_dir(SS_BLANK, true);

    //Set Brightness
    uint8_t ss_currents[] = {255, 238, 0, 255, 255}; //Use this to fine tune brightness of the displays end of chain first
    set_tlc5916_current(ss_currents, DISPLAY_UNITS);

    // Initialize SPI
    spi_init(SS_SPI, SS_SPI_RATE); // Initialize SPI
    gpio_set_function(SS_D, GPIO_FUNC_SPI);
    gpio_set_function(SS_CLK, GPIO_FUNC_SPI);
    spi_set_format(SS_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_slave(SS_SPI, false); // We're the master
    int count = 0;

    // Initialize hardware timer
    struct repeating_timer timer;

    gpio_put(SS_BLANK, 0);
    //boot_text();//display boot message UCOMMENT ME LATER
    
    // Initialize the timer with the given period and enable interrupts
    add_repeating_timer_ms(-100, timer_callback, NULL, &timer);

    sleep_ms(300);//debug sleep
    target_enum();

    while(true){
        //gpio_put(LED_PIN, gpio_get(SW_1));
        gpio_put(TEST_LED_0, 1);
        if (buffer_count > 0) {
            process_uart_data(ring_buffer[read_index]);
            read_index = (read_index + 1) % BUFFER_CAPACITY;
            buffer_count--;
        }

        switch (game_mode)
        {
        case 0://Setup
            setup_mode();
            break;

        case 1://Lights Out
            lights_out_mode();
            break;
        
        case 2://Timer mode
            timer_mode();
            break;
        
        case 3://Blackout mode
            /* code */
            break;
        
        case 4://Attack mode
            /* code */
            break;

        default:
            game_mode = 0;
            break;
        }
    }
    
}