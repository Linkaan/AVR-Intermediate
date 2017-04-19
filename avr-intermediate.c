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

#include <events.h>
#include <fgevents.h>
#include <event2/event.h>

/* Forward declarations used in this file. */
static int fg_event_handler (unsigned char *, size_t, unsigned char *);

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

int
main (void)
{
	ssize_t s;
    int fd;
    struct fg_events_data etdata;

    handle_signals ();

    /* TODO: try connecting over serial to AVR */

    etdata.fg_event_handler = &fg_event_handler;
    s = fg_events_client_init_inet (&etdata, NULL, fd,
                                    MASTER_IP, MASTER_PORT);
    if (s != 0)
      {
        perror ("error initializing fgevents");
        return 1;
      }

    /* TODO: poll on serial fd for any incoming data and send it to
     * master without doing any further processing here */

    /* **************************************************************** */
    /*                                                                  */
    /*                      Begin shutdown sequence                     */
    /*                                                                  */
    /* **************************************************************** */

    fg_events_client_shutdown (&etdata);

    return 0;
}

static int
fg_handle_event (unsigned char *buffer, size_t len, unsigned char *p)
{
    /* TODO: implement writing event serialized directly to avr
}
