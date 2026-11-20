#define WIND_CARRIED_TEST 7

/* A header that a test library carries: C that takes Wind's own values. */

#ifndef WIND_PROGRAM
#include "../../../libs/wind.h"
#endif

static wind_int c_sum(wind_slice_string words, wind_list_char *into) {
    wind_int total = WIND_CARRIED_TEST;
    for (size_t i = 0; i < words.len; i++) {
        total += (wind_int)words.data[i].len;
        for (size_t j = 0; j < words.data[i].len && into->len < into->cap; j++) {
            into->data[into->len++] = (wind_char)words.data[i].data[j];
        }
    }
    return total;
}

static wind_string c_greeting(void) {
    wind_string text = {"carried", 7};
    return text;
}
