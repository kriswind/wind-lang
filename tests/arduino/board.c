/* Stands in for a board, so that the C a sketch is made of can be run on
   the machine running the tests. The serial port is stdin and
   stdout; what the program does to its pins is written out as it happens;
   time passes only in delay. setup() runs the program's top level, and then
   loop() is called LOOPS times. A program that stops after an error ends
   the run with "[halted]". */

#include <stdio.h>
#include <stdlib.h>

#include "Arduino.h"

enum { LOOPS = 3 };

void wind_setup(void);
void wind_loop(void);

static unsigned long now;

void wind_board_write(const char *bytes, size_t count) {
    fwrite(bytes, 1, count, stdout);
}

/* A board waits for input for ever; here input can run out. */
int wind_board_read(void) {
    int c = getchar();
    return c == EOF ? -1 : c;
}

void wind_board_halt(void) {
    fputs("[halted]\n", stdout);
    exit(0);
}

void pinMode(uint8_t pin, uint8_t mode) {
    printf("[pin %d mode %d]", pin, mode);
}

void digitalWrite(uint8_t pin, uint8_t value) {
    printf("[pin %d = %d]", pin, value);
}

int digitalRead(uint8_t pin) {
    return pin % 2;
}

int analogRead(uint8_t pin) {
    return pin * 10;
}

void analogWrite(uint8_t pin, int value) {
    printf("[pin %d duty %d]", pin, value);
}

unsigned long millis(void) {
    return now;
}

unsigned long micros(void) {
    return now * 1000;
}

void delay(unsigned long ms) {
    now += ms;
}

void delayMicroseconds(unsigned int us) {
    (void)us;
}

unsigned long pulseIn(uint8_t pin, uint8_t state, unsigned long timeout) {
    (void)pin;
    (void)state;
    return timeout / 2;
}

void shiftOut(uint8_t data_pin, uint8_t clock_pin, uint8_t bit_order, uint8_t value) {
    printf("[shift %d %d %d %d]", data_pin, clock_pin, bit_order, value);
}

uint8_t shiftIn(uint8_t data_pin, uint8_t clock_pin, uint8_t bit_order) {
    return (uint8_t)(data_pin + clock_pin + bit_order);
}

int main(void) {
    wind_setup();
    for (int i = 0; i < LOOPS; i++) wind_loop();
    return 0;
}
