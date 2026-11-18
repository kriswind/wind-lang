/* Stands in for the Arduino core's header when the C of a sketch is run on
   the machine running the tests. board.c defines what this declares. */
#include <stdint.h>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define LED_BUILTIN 13
#define A0 14
#define A1 15
#define A2 16
#define A3 17
#define A4 18
#define A5 19
#define LSBFIRST 0
#define MSBFIRST 1

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int digitalRead(uint8_t pin);
int analogRead(uint8_t pin);
void analogWrite(uint8_t pin, int value);
unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
unsigned long pulseIn(uint8_t pin, uint8_t state, unsigned long timeout);
void shiftOut(uint8_t data_pin, uint8_t clock_pin, uint8_t bit_order, uint8_t value);
uint8_t shiftIn(uint8_t data_pin, uint8_t clock_pin, uint8_t bit_order);

#define interrupts()
#define noInterrupts()
