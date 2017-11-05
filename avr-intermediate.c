/*
 *  avr-intermediate
 *    Binds together AVR with master by wrapping the serial device in
 *    a fake unix-socket client.
 *  avr-intermediate.c
 *    Set up wiringPi for serial communication and connect to master using
 *    fgevents library
 *****************************************************************************
 *  This file is part of Fågelmataren, an embedded project created to learn
 *  Linux and C. See <https://github.com/Linkaan/Fagelmatare>
 *  Copyright (C) 2015-2017 Linus Styrén
 *
 *  Fågelmataren is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the Licence, or
 *  (at your option) any later version.
 *
 *  Fågelmataren is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public Licence for more details.
 *
 *  You should have received a copy of the GNU General Public Licence
 *  along with Fågelmataren.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************
 */
#include <signal.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <pthread.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <wiringPi/wiringPi.h>
#include <wiringPi/wiringSerial.h>

#include <events.h>
#include <fgevents.h>
#include <event2/event.h>

#define UNIX_SOCKET_PATH "/tmp/fg.socket"

/* Forward declarations used in this file. */
static void fg_event_handler (struct fgevent *, int);
static int read_fgevent_from_serial (unsigned char **, int);

struct thread_data {
    struct fg_events_data etdata;
    int                   fd;
};

/* Pipe used to notify threads to exit from signal handler */
int exitpipe[2];

/* Signal handler for SIGINT, SIGHUP and SIGTERM */
static void
handle_sig (int signum)
{
    struct sigaction new_action;

    /* write to exit pipe */

    new_action.sa_handler = handle_sig;
    sigemptyset (&new_action.sa_mask);
    new_action.sa_flags = SA_RESTART;

    sigaction (signum, &new_action, NULL);
}

/* Setup termination signals to exit gracefully */
static void
handle_signals ()
{
    struct sigaction new_action, old_action;

    /* Turn off buffering on stdout to directly write to log file */
    setvbuf (stdout, NULL, _IONBF, 0);

    /* Set up the structure to specify the new action. */
    new_action.sa_handler = handle_sig;
    sigemptyset (&new_action.sa_mask);
    new_action.sa_flags = SA_RESTART;
    
    /* Handle termination signals but avoid handling signals previously set
       to be ignored */
    sigaction (SIGINT, NULL, &old_action);
    if (old_action.sa_handler != SIG_IGN)
        sigaction (SIGINT, &new_action, NULL);
    sigaction (SIGHUP, NULL, &old_action);
    if (old_action.sa_handler != SIG_IGN)
        sigaction (SIGHUP, &new_action, NULL);
    sigaction (SIGTERM, NULL, &old_action);
    if (old_action.sa_handler != SIG_IGN)
        sigaction (SIGTERM, &new_action, NULL);
}

static int
fgevent_callback (void *arg, struct fgevent *fgev,
                  struct fgevent * UNUSED(ansev))
{
    struct thread_data *tdata = arg;

    /* Handle error in fgevent */
    if (fgev == NULL)
      {
        printf ("error [%s] says %s\n", strerror (tdata->etdata.save_errno), tdata->etdata.error);
        return 0;
      }

    switch (fgev->id) 
      {
        case FG_CONFIRMED:
        case FG_ALIVE:
          break;
        default:
          printf ("got %d event\n", fgev->id);
          fg_event_handler (fgev, tdata->fd);
          break;    
      }

    return 0;
}

int
main (void)
{
    ssize_t s, events;
    int fd;
    struct pollfd poll_fds[2];
    struct thread_data tdata;

    handle_signals ();

    s = wiringPiSetup ();
    if (s < 0)
      {
        perror ("error in wiringPiSetup");
        return 1;
      }

    s = serialOpen ("/dev/ttyAMA0", 9600);
    if (s < 0)
      {
        perror ("error in serialOpen");
        return 1;
      }
    fd = s;

    /* Create a pipe used to singal all threads to begin shutdown sequence */
    s = pipe (exitpipe);
    if (s < 0)
      {
        perror ("error creating pipe");
        return 1;
      }

    tdata.fd = fd;
    s = fg_events_client_init_unix (&tdata.etdata, &fgevent_callback, NULL, &tdata,
                                    UNIX_SOCKET_PATH, FG_AVR);
    if (s != 0)
      {
        errno = s;
        perror ("error initializing fgevents");
      }

    poll_fds[0].fd = fd;
    poll_fds[0].revents = 0;
    poll_fds[0].events = events = POLLIN | POLLPRI;

    poll_fds[1] = poll_fds[0];
    poll_fds[1].fd = exitpipe[0];

    while (1)
      {
        s = poll (poll_fds, 2, -1);

        if (s < 0)
            perror ("poll failed");
        else if (s > 0)
          {
            if (poll_fds[1].revents & events)
              {
                /* exit program */
                break;
              }
            else if (poll_fds[0].revents & events)
              {
                unsigned char *serial_buf;
                s = read_fgevent_from_serial (&serial_buf, fd);

                if (s < 0)
                    perror ("error in read_fgevent_from_serial");
                else if (s > 0)
                  {
                    fg_send_data (&tdata.etdata, serial_buf, s);
                    free (serial_buf);
                  }
              }
          }
      }

    /* **************************************************************** */
    /*                                                                  */
    /*                      Begin shutdown sequence                     */
    /*                                                                  */
    /* **************************************************************** */

    fg_events_client_shutdown (&tdata.etdata);

    return 0;
}

static int
read_fgevent_from_serial (unsigned char **serial_buf_ret, int fd)
{
    size_t serial_size;
    int c, serial_pos;
    unsigned char header_buf[FGEVENT_HEADER_SIZE], *serial_buf;
    struct fgevent header;

    do
      {
        c = serialGetchar (fd);
      }
    while (c > -1 && c != 0x02);

    if (c < 0) return 0;

    for (int i = 0; i < FGEVENT_HEADER_SIZE; i++)
      {
        c = serialGetchar (fd);
        if (c < 0) break;

        header_buf[i] = (unsigned char) c;
      }

    if (c < 0) return 0;

    deserialize_fgevent_header (header_buf, &header);

    printf ("received %d event from avr\n", header.id);

    serial_size = 1 + FGEVENT_HEADER_SIZE + header.length * sizeof (header.payload[0]) + 1;
    serial_buf = malloc (serial_size);
    if (serial_buf == NULL)
      {
        for (int i = 0; i < header.length + 1; i++)
          {
            c = serialGetchar (fd);
            if (c < 0) break;
          }
        return -1;
      }

    serial_buf[0] = 0x02;
    for (serial_pos = 1; serial_pos < (ssize_t) serial_size - 2; serial_pos++)
      {
        if (serial_pos < FGEVENT_HEADER_SIZE+1)
          {
            serial_buf[serial_pos] = header_buf[serial_pos-1];
            continue;
          }

        c = serialGetchar (fd);
        if (c < 0) break;

        serial_buf[serial_pos] = (unsigned char) c;
      }
    serial_buf[serial_pos+1] = 0x03;

    if (c < 0)
      {
        free (serial_buf);
        return 0;
      }

    do
      {
        c = serialGetchar (fd);
      }
    while (c > -1 && c != 0x03);

    *serial_buf_ret = serial_buf;

    return serial_size;
}

static void
fg_event_handler (struct fgevent *fgev, int fd)
{
    ssize_t s;
    unsigned char *fgbuf;    
    
    s = create_serialized_fgevent_buffer (&fgbuf, fgev);
    if (s < 0)
        return;

    for (int i = 0; i < s; i++)
      {
        serialPutchar (fd, fgbuf[i]);
      }

    free (fgbuf);
}
