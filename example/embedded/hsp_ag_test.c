/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */
 
// *****************************************************************************
//
// Minimal test for HSP Audio Gateway (!! UNDER DEVELOPMENT !!)
//
// *****************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>


#include <btstack/hci_cmds.h>
#include <btstack/run_loop.h>
#include <btstack/sdp_util.h>

#include "hci.h"
#include "l2cap.h"
#include "sdp.h"
#include "debug.h"
#include "hsp_ag.h"

static uint8_t    hsp_service_buffer[150];
static uint8_t    rfcomm_channel_nr = 1;

static uint8_t ag_speaker_gain = 0;
static uint8_t ag_microphone_gain = 0;

static char hsp_ag_service_name[] = "Audio Gateway Test";
static bd_addr_t bt_speaker_addr = {0x00, 0x21, 0x3C, 0xAC, 0xF7, 0x38};

static char hs_cmd_buffer[100];
// prototypes
static void show_usage();


// Testig User Interface 
static data_source_t stdin_source;

static uint8_t next_ag_microphone_gain(){
    ag_microphone_gain++;
    if (ag_microphone_gain > 15) ag_microphone_gain = 0;
    return ag_microphone_gain;
}

static uint8_t next_ag_speaker_gain(){
    ag_speaker_gain++;
    if (ag_speaker_gain > 15) ag_speaker_gain = 0;
    return ag_speaker_gain;
}

static void show_usage(){

    printf("\n--- Bluetooth HSP AudioGateway Test Console ---\n");
    printf("---\n");
    printf("e - establish audio connection to Bluetooth Speaker\n");
    printf("r - release audio connection from Bluetooth Speaker\n");
    printf("m - set microphone gain\n");
    printf("s - set speaker gain\n");
    printf("---\n");
    printf("Ctrl-c - exit\n");
    printf("---\n");
}

static int stdin_process(struct data_source *ds){
    char buffer;
    read(ds->fd, &buffer, 1);

    switch (buffer){
        case 'e':
            printf("Establishing audio connection to Bluetooth Speaker %s...\n", bd_addr_to_str(bt_speaker_addr));
            hsp_ag_connect(bt_speaker_addr);
            break;
        case 'r':
            printf("Releasing audio connection from Bluetooth Speaker %s...\n", bd_addr_to_str(bt_speaker_addr));
            hsp_ag_disconnect();
            break;
        case 'm':
            printf("Setting microphone gain\n");
            hsp_ag_set_microphone_gain(next_ag_microphone_gain());
            break;
        case 's':
            printf("Setting speaker gain\n");
            hsp_ag_set_speaker_gain(next_ag_speaker_gain());
            break;
        default:
            show_usage();
            break;

    }
    return 0;
}

static void setup_cli(){

    struct termios term = {0};
    if (tcgetattr(0, &term) < 0)
            perror("tcsetattr()");
    term.c_lflag &= ~ICANON;
    term.c_lflag &= ~ECHO;
    term.c_cc[VMIN] = 1;
    term.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &term) < 0)
            perror("tcsetattr ICANON");

    stdin_source.fd = 0; // stdin
    stdin_source.process = &stdin_process;
    run_loop_add_data_source(&stdin_source);

    show_usage();
}

// Audio Gateway routines 
void packet_handler(uint8_t * event, uint16_t event_size){
    switch (event[2]) {
        case HSP_SUBEVENT_AUDIO_CONNECTION_COMPLETE:
            if (event[3] == 0){
                printf("Audio connection established.\n");
            } else {
                printf("Audio connection establishment failed with status %u\n", event[3]);
            }
            break;
        case HSP_SUBEVENT_AUDIO_DISCONNECTION_COMPLETE:
            if (event[3] == 0){
                printf("Audio connection released.\n");
            } else {
                printf("Audio connection releasing failed with status %u\n", event[3]);
            }
            break;
        case HSP_SUBEVENT_MICROPHONE_GAIN_CHANGED:
            printf("Received microphone gain change %d\n", event[3]);
            break;
        case HSP_SUBEVENT_SPEAKER_GAIN_CHANGED:
            printf("Received speaker gain change %d\n", event[3]);
            break;
        case HSP_SUBEVENT_HS_COMMAND:{
            memset(hs_cmd_buffer, 0, sizeof(hs_cmd_buffer));
            int size = event_size <= sizeof(hs_cmd_buffer)? event_size : sizeof(hs_cmd_buffer); 
            memcpy(hs_cmd_buffer, &event[3], size - 1);
            printf("Received command %s\n", hs_cmd_buffer);
            break;
        }
        default:
            break;
    }
}

int btstack_main(int argc, const char * argv[]){
    // init SDP, create record for SPP and register with SDP
    memset((uint8_t *)hsp_service_buffer, 0, sizeof(hsp_service_buffer));
    hsp_ag_create_service((uint8_t *)hsp_service_buffer, rfcomm_channel_nr, hsp_ag_service_name);
    
    sdp_init();
    sdp_register_service_internal(NULL, (uint8_t *)hsp_service_buffer);

    hsp_ag_init(rfcomm_channel_nr);
    hsp_ag_register_packet_handler(packet_handler);
    // turn on!
    hci_power_control(HCI_POWER_ON);

    setup_cli();
    // go!
    run_loop_execute(); 
    return 0;
}
