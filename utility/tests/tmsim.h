#ifndef TMSIM_H
#define TMSIM_H
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int     fd;
    int     noise;            /* inject ASB-like bytes before replies */
    int     in_setting;
    int     resets;
    int     ignored_writes;   /* fn3/fn5 outside user setting mode    */
    long    other_bytes;
    long    custom[256];
    uint8_t msw[9];
    uint8_t status[5];
} tm_sim;

int tm_sim_start(tm_sim *s, int fd);
#endif
