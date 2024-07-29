
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/sem.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"

#define LED_PIN PICO_DEFAULT_LED_PIN
#define TEST_LED_0 2
#define SW_1 28
#define SW_2 27
#define SW_3 26

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

#define MAX_PLAYERS 4 //Set for number of player scoreboards in the chain
#define DISPLAY_UNITS MAX_PLAYERS + 1 //Total number of displays with clock
volatile int score[MAX_PLAYERS] = {0};

volatile bool tiger_1;
volatile int led_on_timer =0;
volatile int piezo_trig_cnt =0;
volatile uint game_time=0;

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


int ss_int_write(int num){
    uint8_t data_to_send [4];
    data_to_send [3] = segments[num % 10];
    data_to_send [2] = segments[(num /10) % 10];
    data_to_send [1] = segments[(num /100) % 10];
    data_to_send [0] = segments[(num /1000) % 10];
    spi_write_blocking(SS_SPI, &data_to_send[0], 4); // Send one byte
}
int ss_time_write(int num){
    uint8_t data_to_send [4];
    uint8_t blink = ((num % 10)>4) ? 0x00 : 0x80; //blink ":" between min and sec
    int seconds = (num/10)%60;
    data_to_send [3] = segments[num % 10];
    data_to_send [2] = segments[(seconds) % 10] | 0x80;
    data_to_send [1] = segments[(seconds /10) % 10]| blink;
    data_to_send [0] = segments[(num /600) % 10] | blink;
    spi_write_blocking(SS_SPI, &data_to_send[0], 4); // Send one byte
}

bool timer_callback (struct repeating_timer *t) {
    gpio_put(SS_LAT, 0);
    if (game_time != 0)
    {
        game_time--;
    }
    ss_time_write(game_time);
    for (size_t i = 0; i < MAX_PLAYERS; i++)
        {
            ss_int_write(score[i]);
        }
    gpio_put(SS_LAT, 1);
    return true;
}

// void button_callback(){
// }


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

    // Initialize RS232
    uart_init(uart0, BAUD_RATE);
    gpio_set_function(UART0_TX, GPIO_FUNC_UART);
    gpio_set_function(UART0_RX, GPIO_FUNC_UART);   

    //Seven segmet outputs
    gpio_init(SS_LAT);
    gpio_set_dir(SS_LAT, true);
    gpio_init(SS_BLANK);
    gpio_set_dir(SS_BLANK, true);

    //Set Brightness
    uint8_t ss_currents[] = {254, 238, 0, 255, 255}; //Use this to fine tune brightness of the displays end of chain first
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

    // Set up interrupt for falling edge on button pin
    //gpio_set_irq_enabled_with_callback(SW_1, GPIO_IRQ_EDGE_RISE, true, &button_callback);

    while(true){
        gpio_put(LED_PIN, gpio_get(SW_1));
        gpio_put(TEST_LED_0, 1);
        if(!gpio_get(SW_1)){
            game_time = 1800;
            target_on(1, 0, score[1]%4, score[0]%4);
            //target_on(1, 1, 0, (score[0]+1)%4);
            target_on(1, 1, (score[1]+1)%4, (score[0]+1)%4);
            // target_on(1, 2, 2, 2);
            // target_on(1, 3, 3, 3);
            //printf("Button Press\n");
        }

        score[0]++;
        score[1]+=3;
        score[2]+=5;
        sleep_ms(300);
    }
    
}