/*
 * Generic cm160 to MQTT
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *
 * Based on the eagle-owl application by Philippe Cornet <phil.cornet@gmail.com>
 * https://github.com/cornetp/eagle-owl
 *
 * Based on the "morpork" application by Ben Jones  <ben.jones12@gmail.com>
 * <https://groups.google.com/g/openhab/c/G4_yariNZRk/m/IYo0j6cRCBwJ
 *
 *
 * To build
 *
 *   apt install libusb-dev libmosquitto-dev
 *   gcc *.c -Wall -o cm160 -lmosquitto -lusb
 *   (or  gcc *.c -Wall -o cm160 -lmosquitto /usr/lib/x86_64-linux-gnu/libusb.a)
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <inttypes.h>
#include <usb.h>
#include <mosquitto.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <usb.h>
#include "cm160.h"

// cm160 control messages
#define OWL_VENDOR_ID 0x0fde
#define CM160_DEV_ID  0xca05
static char ID_MSG[11] = { 0xA9, 0x49, 0x44, 0x54, 0x43, 0x4D, 0x56, 0x30, 0x30, 0x31, 0x01 };
static char WAIT_MSG[11] = { 0xA9, 0x49, 0x44, 0x54, 0x57, 0x41, 0x49, 0x54, 0x50, 0x43, 0x52 };

// list of cm160 devices
struct cm160_device devices[MAX_DEVICES];

struct mosquitto *mosq = NULL;
int mqtt_port = 1883;
char mqtt_server[100];
char mqtt_topic[100];
char mqtt_announce_topic[100];
char hostname[100];
char *programname;
int voltage = 240;       // voltage used to calculate watts (Owl reports amps only)
int debug = 0;
static volatile int active = 1;
extern struct cm160_device devices[MAX_DEVICES];


static uint64_t millis() {
    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t millis = ((uint64_t)time.tv_sec * 1000) + (time.tv_usec / 1000ULL);
    return millis;
}

static void decode_live_frame(unsigned char *data, struct cm160_frame *frame) {
    static int last_valid_month = 0; 

    frame->id = data[0];
    frame->year = data[1] + 2000;
    frame->month = data[2];
    frame->day = data[3];
    frame->hour = data[4];
    frame->min = data[5];
    frame->cost = (data[6]+(data[7]<<8)) / 100.0;
    frame->amps = (data[8]+(data[9]<<8)) * 0.07; // mean intensity during one minute
    frame->watts = frame->amps * voltage; // mean power during one minute

    // sometimes a bad month is received so just use the last valid month
    if (frame->month < 0 || frame->month > 12) {
        frame->month = last_valid_month;
    } else {
        last_valid_month = frame->month;
    }
    // Live data - we don't care about what date the system thinks it is.
    char buf[200];
    sprintf(buf, "{\"type\":\"cm160\",\"amps\":%1.2f,\"watts\":%d,\"when\":%" PRIu64 ",\"who\":\"%s\",\"where\":\"%s\"}", frame->amps, (int)frame->watts, millis()/1000, programname, hostname);
    mosquitto_publish(mosq, NULL, mqtt_topic, strlen(buf), buf, 0, 0);
    printf("%s\n", buf);
}

static int process_frame(int dev_id, unsigned char *data) {
    unsigned char send_data[1];
    unsigned int checksum = 0;
    usb_dev_handle *hdev = devices[dev_id].hdev;
    int epout = devices[dev_id].epout;
    int i;
   
    if (strncmp((char *)data, ID_MSG, 11) == 0) {
        if (debug) {
            printf("DEBUG: Received ID frame\n");
        }
        send_data[0] = 0x5A;
        usb_bulk_write(hdev, epout, (const char *)&send_data, sizeof(send_data), 1000);
    } else if (strncmp((char *)data, WAIT_MSG, 11) == 0) {
        if (debug) {
            printf("DEBUG: Received WAIT frame\n");
        }
        send_data[0] = 0xA5;
        usb_bulk_write(hdev, epout, (const char *)&send_data, sizeof(send_data), 1000);
    } else if (data[0] == FRAME_ID_HISTORY) {
        if (debug) {
            printf("DEBUG: Received HISTORY frame\n");
        }
    } else if (data[0] == FRAME_ID_LIVE) {
        if (debug) {
            printf("DEBUG: Received LIVE frame\n");
        }
        // calculate and check the checksum
        for (i=0; i<10; i++) {
            checksum += data[i];
        }
        checksum &= 0xff;
        if (checksum != data[10]) {
            if (debug) {
                printf("DEBUG: data error: invalid checksum: expected 0x%x, got 0x%x\n", data[10], checksum);
            }
            return -1;
        }
        struct cm160_frame frame;
        decode_live_frame(data, &frame);
    } else if (debug) {
        printf("DEBUG: data error: invalid ID 0x%x\n", data[0]);
        for (i=0; i<11; i++) {
            fprintf(stderr, "0x%02x - ", data[i]);
        }
        fprintf(stderr, "\n");
        return -1;
    }
    return 0;
}

static int io_loop(int dev_id) {
    usb_dev_handle *hdev = devices[dev_id].hdev;
    int epin = devices[dev_id].epin;

    unsigned char buffer[11];
    int ret;
    while (active) {
        memset(buffer, 0, sizeof(buffer));
        ret = usb_bulk_read(hdev, epin, (char*)buffer, sizeof(buffer), 6000);   // Timeout seems to matter! Reducing we get error -110 (error sending control message: Bad address)

        if (ret == 11) {
            process_frame(dev_id, buffer);
        } else if (ret < 11) {
            fprintf(stderr, "bulk_read returned %d (%s)\n", ret, usb_strerror());
            return -1;
        } else {
            fprintf(stderr, "partial read returned %d\n", ret);
        }
    }
    return 0;
}

static int handle_device(int dev_id) {
    int r, i;
    struct usb_device *dev = devices[dev_id].usb_dev;
    usb_dev_handle *hdev = devices[dev_id].hdev;

    usb_detach_kernel_driver_np(hdev, 0);
    
    if (0 != (r = usb_set_configuration(hdev, dev->config[0].bConfigurationValue))) {
        fprintf(stderr, "usb_set_configuration returns %d (%s)\n", r, usb_strerror());
        return -1;
    }

    // claim the usb interface
    if ((r = usb_claim_interface(hdev, 0)) < 0) {
        fprintf(stderr, "usb interface cannot be claimed: %d\n", r);
        return r;
    }

    // get handles to the in/out end points
    int nep = dev->config->interface->altsetting->bNumEndpoints;
    for (i=0; i<nep; i++) {
        int ep = dev->config->interface->altsetting->endpoint[i].bEndpointAddress;
        if (ep&(1<<7)) {
            devices[dev_id].epin = ep;
        } else {
            devices[dev_id].epout = ep;
        }
    }

    // set baudrate
    r = usb_control_msg(hdev, USB_TYPE_VENDOR | USB_RECIP_INTERFACE | USB_ENDPOINT_OUT, CP210X_IFC_ENABLE, UART_ENABLE, 0, NULL, 0, 500);
    r = usb_control_msg(hdev, USB_TYPE_VENDOR | USB_RECIP_INTERFACE | USB_ENDPOINT_OUT, CP210X_SET_BAUDRATE, 0, 0, (char *)CM160_BAUD_RATE, sizeof(CM160_BAUD_RATE), 500);
    r = usb_control_msg(hdev, USB_TYPE_VENDOR | USB_RECIP_INTERFACE | USB_ENDPOINT_OUT, CP210X_IFC_ENABLE, UART_DISABLE, 0, NULL, 0, 500);
    
    // read/write main loop
    io_loop(dev_id);
   
    // release the usb interface
    usb_release_interface(hdev, 0);
    
    return 0;
}

void usage() {
    printf("Usage: %s [--host <mqtt-server>] [--port <mqtt-port>] [--topic <mqtt-topic>] [--announce-topic <mqtt-topic>] [--voltage <voltage>]\n\n", programname);
    exit(-1);
}

void cleanup() {
}

static void mqttConnect(struct mosquitto *mosq, void *obj, int rc) {
    if (rc) {
        printf("MQTT: connect to %s:%d failed: %d\n", mqtt_server, mqtt_port, rc);
    } else {
        printf("MQTT: connected to %s:%d\n", mqtt_server, mqtt_port);
    }
}

void cancel() {
    active = 0;
}

int scan_usb() {
    int dev_cnt = 0;
    usb_init();
    usb_find_busses();
    usb_find_devices();
    struct usb_bus *bus = NULL;
    struct usb_device *dev = NULL;
    for (bus = usb_get_busses(); bus; bus = bus->next) {
        for (dev = bus->devices; dev; dev=dev->next) {
            if (dev->descriptor.idVendor == OWL_VENDOR_ID && dev->descriptor.idProduct == CM160_DEV_ID && dev_cnt < MAX_DEVICES) {
                printf("CM160: found compatible device #%d: %04x:%04x (%s)\n", dev_cnt, dev->descriptor.idVendor, dev->descriptor.idProduct, dev->filename);
                devices[dev_cnt].usb_dev = dev;
                dev_cnt++;
            }
        }
    }
    return dev_cnt;
}

int main(int argc, char **argv) {
    programname = argv[0];
    strcpy(mqtt_server, "localhost");
    strcpy(mqtt_topic, "cm160");
    for (int i=1;i<argc;i++) {
        if (!strcmp("--host", argv[i]) && i + 1 < argc) {
            strncpy(mqtt_server, argv[++i], sizeof(mqtt_server));
        } else if (!strcmp("--port", argv[i])) {
            char *c;
            int v = strtol(argv[++i], &c, 10);
            if (*c || v <= 0 || v > 65535) {
                printf("Invalid port \"%s\" (1..65535)\n", argv[i]);
                usage();
            }
            mqtt_port = v;
        } else if (!strcmp("--topic", argv[i]) && i + 1 < argc) {
            strncpy(mqtt_topic, argv[++i], sizeof(mqtt_topic));
        } else if (!strcmp("--announce-topic", argv[i]) && i + 1 < argc) {
            strncpy(mqtt_announce_topic, argv[++i], sizeof(mqtt_announce_topic));
        } else if (!strcmp("--voltage", argv[i]) && i + 1 < argc) {
            char *c;
            int v = strtol(argv[++i], &c, 10);
            if (*c || v <= 0 || v > 500) {
                printf("Invalid voltage \"%s\" (1..500)\n", argv[i]);
                usage();
            }
            voltage = v;
        } else if (!strcmp("--debug", argv[i])) {
            debug = 1;
        } else {
            usage();
        }
    }

    char buf[200];
    gethostname(hostname, 100);
    int keepalive = 60;
    bool clean_session = true;
    mosquitto_lib_init();
    mosq = mosquitto_new(NULL, clean_session, NULL);
    if (!mosq) {
        perror("ERROR: Mosquitto init failed");
        exit(-1);
    }
//    mosquitto_log_callback_set(mosq, mqttLog);
//    mosquitto_disconnect_callback_set(mosq, mqttDisconnect);
    mosquitto_connect_callback_set(mosq, mqttConnect);
    if (strlen(mqtt_announce_topic)) {
        char buf[180];
        sprintf(buf, "{\"type\":\"announce\",\"connect\":false,\"who\":\"%s\",\"where\":\"%s\"}", argv[0], hostname);
        mosquitto_will_set(mosq, mqtt_announce_topic, strlen(buf), buf, 0, false);
    }
    if (mosquitto_connect(mosq, mqtt_server, mqtt_port, keepalive)) {
        perror("ERROR: Mosquitto connect failed");
        exit(-1);
    }
    if (mosquitto_loop_start(mosq) != MOSQ_ERR_SUCCESS) {
        perror("ERROR: Mosquitto loop failed");
        exit(-1);
    }
    if (strlen(mqtt_announce_topic)) {
        sprintf(buf, "{\"type\":\"announce\",\"connect\":true,\"when\":%" PRIu64 ",\"who\":\"%s\",\"where\":\"%s\"}", millis()/1000, argv[0], hostname);
        mosquitto_publish(mosq, NULL, mqtt_announce_topic, strlen(buf), buf, 0, 0);
    }
    signal(SIGINT, cancel);
    signal(SIGTERM, cancel);

    int dev_cnt;
    while (active) {
        dev_cnt = 0;
        while (active && (dev_cnt = scan_usb()) == 0) {
            sleep(2);
        }
        if (dev_cnt == 1) {
            // only 1 device supported
            if (!(devices[0].hdev = usb_open(devices[0].usb_dev))) {
                fprintf(stderr, "failed to open device\n");
                break;
            }
            handle_device(0); 
        }
    }
    if (dev_cnt == 1) {
        usb_close(devices[0].hdev);
    }
    if (mosq) {
        mosquitto_destroy(mosq);
    }
    mosquitto_lib_cleanup();
    return 0;
}
