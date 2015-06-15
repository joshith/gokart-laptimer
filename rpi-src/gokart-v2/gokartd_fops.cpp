/*
 ******************************************************************************
 ******************************************************************************
 ***Gokartd - raspi conroller daemon for gokart lap-detector-iMproved (GLTDiM**
 ***gokartd.cpp ***************************************************************
 ***Copyright (C) 2015 Joshith (joe.cet@gmail.com)*****************************
 ******************************************************************************
 ******************************************************************************
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 version 2 as published by the Free Software Foundation.
 */
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include "gokartd.h"

extern kart_data_t         rx_data[MAX_PAYLOAD_COUNT];
extern uint16_t            rx_data_count;

karts_t all_karts[MAX_KARTS][MAX_DET_TYPE][MAX_DET_CODE];

void
gokart_snapapic(uint8_t kart_id, struct timeval *tv) {
    pid_t pid = 0;
    char img_path[128] = "";

    snprintf(img_path, 64, "%s_kart%d_%lu.jpg", PISTILLARGS, kart_id, tv->tv_sec);

    log_print ("raspistill arg path: %s\n", img_path);
    pid = fork ();

    if (pid == -1) {
        /* fork failed */
        return;
    }

    if (pid > 0) {
        /*this is parent */
        return;
    } else {
        /* this is child */
        if(-1 == execv(PISTILLPATH, (char * const *)img_path)) {
            log_print ("raspistill exec failed path:%s arg path: %s\n", PISTILLPATH, img_path);
        }
        exit (1);
    }

}

int check_fd_fine(FILE *fp) {
    struct stat _stat;
    int ret = -1;
    int fd  =  0;

    if (!fp)
        return -1;

    fd = fileno(fp);

    if(fd > 1) {
        if(!fstat(fd, &_stat)) {
            if(_stat.st_nlink >= 1) {
                return 0;
            }
            log_print ("file removed fd: %d st_nlink:%d\n", fd, _stat.st_nlink);
        }
    }
    log_print ("failure fd: %d\n", fd);
    return ret;
}


int
gokart_process_data(void)
{
    kart_data_t *one_data = NULL;
   // int         data_count = rx_data_count;

    karts_t        *this_kart = NULL;
    struct timeval tv;
    char           file_name[64] = "";
    FILE           *fp = NULL;

    if (!rx_data_count) {      //early return for empty list
        return -1;
    }

    log_print("process_data:ENTER: count:%d \n", rx_data_count);

    do {
        rx_data_count--;
        one_data = &rx_data[rx_data_count];

        if (one_data->dev_id < 0 || one_data->dev_id >= MAX_KARTS) { // Error
            log_print ("invalid Kart_id: %d \n", one_data->dev_id);
            continue;
        }

        /* Now get the kart struct */
        this_kart = &all_karts[one_data->dev_id][one_data->detect_type][one_data->detect_code];

        /* Fix the time issue here */
        tv.tv_sec = one_data->time.sec/2; //Each tick is .5Sec
        tv.tv_usec = (500*(one_data->time.sec%2) + one_data->time.m_sec*2); //Each tick is 2mSec

        this_kart->kart_id = one_data->dev_id;
        this_kart->battery_level    = (one_data->battery_level*100/255);    //Convert to n%
        this_kart->detect_type      = one_data->detect_type;
        this_kart->detect_code      = one_data->detect_code;

        log_print ("LOG: dev_id:%d, type:%s, code: 0x%x battery_level:%d%% time:%dS:%dmS\n",
                this_kart->kart_id, (this_kart->detect_type?"HALL":"IR"), this_kart->detect_code,
                                    (int)this_kart->battery_level, (int)tv.tv_sec, (int)tv.tv_usec);

        /* New whole set session or
         * individual kart session (only file for this kart was deleted) */
        if (this_kart->kart_id == 0 || check_fd_fine (this_kart->fp)) {
            /* We are seeing this kart for the first time */
           // strncpy (this_kart->kart_name, one_data->dev_id, 20);

            this_kart->num_laps         = 0;
            this_kart->lap_time         = this_kart->new_lap_time = tv;

            sprintf(file_name, "KART%d_%s_%d", this_kart->kart_id,
                (this_kart->detect_type?"HALL":"IR"), this_kart->detect_code);

            /* open a file for data */
            if ((fp = fopen (file_name, "w+")) == NULL) {       //FIXME: type detect
                log_print ("file open with name : %s failed\n", file_name);
                continue;
            }

            log_print ("new session detected. file_name: %s\n", file_name);

            this_kart->fp = fp;
            fprintf (fp, "KART:%d:START_TIME:%d.%d:END_TIME:%d.%d:BAT:%d:DET_TYPE:%d:DET_CODE:%d:\n",
                this_kart->kart_id, 0, 0, (int)this_kart->new_lap_time.tv_sec, (int)this_kart->new_lap_time.tv_usec,
                this_kart->battery_level, this_kart->detect_type, this_kart->detect_code);

            fflush (this_kart->fp);

            /* take a snap */
            gokart_snapapic (this_kart->kart_id, &(this_kart->lap_time));
            /* ALL success */
            continue;
        }

        /* subsequent lap */
        if (this_kart->kart_id != one_data->dev_id) {
            /* Error */
            log_print ("inconsistent data. kart_id db: %x, kart_id signal: %x",
                    this_kart->kart_id, one_data->dev_id);
            continue;
        }

        /* Check if this is a duplicate data.
         * Wraparound time for timer is 9Hrs.
         * Assuming, one session wouldn't last that long - no wraparound handling logic */
        if (this_kart->lap_time.tv_sec >= tv.tv_sec){
            log_print ("kart:%d: duplicate lap data, ignore\n", this_kart->kart_id);
            continue;
        }

        this_kart->lap_time = this_kart->new_lap_time ;
        this_kart->new_lap_time = tv;
        this_kart->num_laps++;

        fprintf (this_kart->fp, "KART:%d:START_TIME:%d.%d:END_TIME:%d.%d:BAT:%d:DET_TYPE:%d:DET_CODE:%d:\n",
              this_kart->kart_id, (int)this_kart->lap_time.tv_sec, (int)this_kart->lap_time.tv_usec,
              (int)tv.tv_sec, (int)tv.tv_usec, this_kart->battery_level,
              this_kart->detect_type, this_kart->detect_code);

        fflush (this_kart->fp);

        /* take a snap */
        gokart_snapapic (this_kart->kart_id, &(this_kart->lap_time));
    } while (rx_data_count);

    return (0);
}
