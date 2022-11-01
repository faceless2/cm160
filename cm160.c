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

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/time.h>
#include <inttypes.h>
#include <mosquitto.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <libusb-1.0/libusb.h>

#define OWL_VENDOR_ID           0x0fde
#define CM160_DEV_ID            0xca05
#define USB_INTERFACE           0
#define USB_CONFIGURATION       1
#define USB_ENDPOINT_IN	        (LIBUSB_ENDPOINT_IN  | 1)   /* endpoint address */
#define USB_ENDPOINT_OUT	(LIBUSB_ENDPOINT_OUT | 2)   /* endpoint address */
// CP210X device options
#define CP210X_IFC_ENABLE       0x00
#define CP210X_GET_LINE_CTL     0x04
#define CP210X_SET_MHS          0x07
#define CP210X_GET_MDMSTS       0x08
#define CP210X_GET_FLOW         0x14
#define CP210X_GET_BAUDRATE     0x1D
#define CP210X_SET_BAUDRATE     0x1E
// CP210X_IFC_ENABLE
#define UART_ENABLE             0x0001
#define UART_DISABLE            0x0000
// CM160 protocol
#define BULK_ENDPOINT_IN        0x82
#define BULK_ENDPOINT_OUT       0x01
#define FRAME_ID_CONTROL        0xA9
#define FRAME_ID_LIVE           0x51
#define FRAME_ID_HISTORY        0x59
#define FRAME_ID_UNKNOWN        0x1D
#define FRAME_SIZE              11
#define MAXIDCOUNT              8

static char ID_MSG[11] =   { 0xA9, 0x49, 0x44, 0x54, 0x43, 0x4D, 0x56, 0x30, 0x30, 0x31, 0x01 };                // {A9}IDTCMV001{01}
static char WAIT_MSG[11] = { 0xA9, 0x49, 0x44, 0x54, 0x57, 0x41, 0x49, 0x54, 0x50, 0x43, 0x52 };                // {A9}IDTWAITPCR

typedef struct cm160_struct {
    struct libusb_transfer *transfer_in;
    struct libusb_transfer *transfer_out;
    struct libusb_device_handle *devh;
    uint8_t buf[4096];
    int buflen;
    uint8_t idcount;
    uint8_t seenlivedata, kernel;
    struct cm160_struct *next;
} cm160_t;

// list of cm160 devices
cm160_t *head;
struct mosquitto *mosq = NULL;
int mqtt_port = 1883;
char mqtt_server[100];
char mqtt_topic[100];
char mqtt_announce_topic[100];
char hostname[100];
char *programname;
int voltage = 230;       // voltage used to calculate watts (Owl reports amps only)
int debug = 0;
static volatile int active = 1;


static uint64_t millis() {
    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t millis = ((uint64_t)time.tv_sec * 1000) + (time.tv_usec / 1000ULL);
    return millis;
}

int process_frame(cm160_t *cm160) {
    if (debug) {
        char buf[12];
        time_t timer = time(NULL);
        struct tm *tm = localtime(&timer);
        strftime(buf, sizeof(buf), "%H:%M:%S", tm);
        printf("DEBUG: %s  ", buf);
        for (int i=0; i<11; i++) {
            buf[i] = cm160->buf[i];
            if (buf[i] < 0x30 || buf[i] >= 0x80) {
                buf[i] = '.';
            }
            printf("%02x ", cm160->buf[i]);
        }
        buf[11] = 0;
        printf("   %s  ", buf);
    }

    if (!memcmp(cm160->buf, ID_MSG, 11)) {
        unsigned char send = 0x5a;
        if (debug) {
            printf("ID frame: replying 0x%x\n", send);
        }
        int r, i;
        if ((r=libusb_bulk_transfer(cm160->devh, BULK_ENDPOINT_OUT, &send, 1, &i, 1000))) {
            printf("ERROR: libusb_bulk_transfer returned %d (%s)\n", r, libusb_strerror(r));
        } else if (i != 1) {
            printf("ERROR: libusb_bulk_transfer sent %d\n", i);
        }
        cm160->idcount++;
        return 11;

    } else if (!memcmp(cm160->buf, WAIT_MSG, 11)) {
        cm160->idcount = 0;
        cm160->seenlivedata = 1;
        unsigned char send = 0xa5;
        if (debug) {
            printf("Wait frame: replying 0x%x\n", send);
        }
        int r, i;
        if ((r=libusb_bulk_transfer(cm160->devh, BULK_ENDPOINT_OUT, &send, 1, &i, 1000)) < 0) {
            printf("ERROR: libusb_bulk_transfer returned %d (%s)\n", r, libusb_strerror(r));
        } else if (i != 1) {
            printf("ERROR: libusb_bulk_transfer sent %d\n", i);
        }
        return 11;

    } else if (cm160->buf[0] == FRAME_ID_HISTORY || cm160->buf[0] == FRAME_ID_LIVE) {
        // Original "eagle-owl" program decided 0x59 was "History" and 0x51 was "current.
        // But not quite right. 0x51 is "historical or no change from previous record"
        // and 0x59 means the reading is current AND the read value is different to 
        // the value read last time.
        //
        // See also https://sourceforge.net/p/electricowl/discussion/1083264/thread/7f01752f/?limit=25#693c
        // 

        // History frames are sometimes 11 bytes, sometimes 10.
        // Could this be the device itself? Seems to be missing
        // the "minute" byte.
        //
        // Sample responses with 11 bytes
        // H  YR M  D  HR MI COST  AMPS   
        // 59 0b 81 01 14 1f c4 09 44 00 2a
        // 59 0b c1 01 14 20 c4 09 44 00 6b
        // 59 0b c1 01 14 14 c4 09 45 00 60
        // 59 0b c1 01 14 16 c4 09 45 00 62
        // 59 0b c1 01 14 1d c4 09 44 00 68
        // 59 0b c1 01 14 14 c4 09 45 00 60
        // 59 0b c1 01 14 07 c4 09 43 00 51
        // Sample responses with 10 bytes
        // 59 0b c1 01 14    c4 09 43 00 5d
        // 59 0b 81 01 14    c4 09 43 00 1d
        // 59 0b c1 01 38    c4 09 43 00 81
        // 59 0b c1 01 39    c4 09 44 00 83
        // 59 0b c1 01 3b    c4 09 40 00 81
        //
        // Now srongly suspect that this indicates the USB port is in the wrong serial mode, somehow
        // Fix with ioctl? Would be nice to fix with libusb but that means wading into 
        // https://github.com/torvalds/linux/blob/master/drivers/usb/serial/cp210x.c

        cm160->idcount = 0;
        bool newdata = cm160->buf[0] == FRAME_ID_LIVE || (cm160->buf[2] & 0x40) == 0;
        int checksum = 0;
        for (int i=0;i<10;i++) {
            checksum += cm160->buf[i];
        }
        checksum &= 0xff;
        if (checksum == cm160->buf[10]) {
            if (debug) {
                if (newdata) {
                    printf("Live frame\n");
                } else {
                    printf("History frame\n");
                }
            }
            // eagle-owl said bytes were
            //  1       year - 2000
            //  2       month [0..11] with flags: 0x80 means "cost should be multiplied by 100", 0x40 means "data available"
            //  3       day of month
            //  4       hour
            //  5       minute
            //  6,7     cost (it's actually current tarrif), little-endian
            //  8,9     amps, little-endian * 0.07
            //
            if (newdata) {
                cm160->seenlivedata = 1;
            }
            cm160->seenlivedata = 1;

            if (cm160->seenlivedata) {
                float amps = (cm160->buf[8] + (cm160->buf[9]<<8)) * 0.07; // mean intensity during one minute
                float watts = amps * voltage; // mean power during one minute
                char buf[200];
                sprintf(buf, "{\"type\":\"cm160\",\"amps\":%1.2f,\"watts\":%d,\"when\":%" PRIu64 ",\"who\":\"%s\",\"where\":\"%s\"}", amps, (int)watts, millis()/1000, programname, hostname);
                mosquitto_publish(mosq, NULL, mqtt_topic, strlen(buf), buf, 0, 0);
                printf("%s\n", buf);
            }

            return 11;
        } else {
            if (debug) {
                if (newdata) {
                    printf("Live frame (bad checksum: expected 0x%x)\n", checksum);
                } else {
                    printf("History frame (bad checksum: presuming 10-byte record)\n");
                }
            }
            return newdata ? 0 : 10;
        }

    } else {
        cm160->idcount = 0;
        printf("Unknown frame\n");
        return 0;
    }
}

void usage() {
    printf("Usage: %s [--host <mqtt-server>] [--port <mqtt-port>] [--topic <mqtt-topic>] [--announce-topic <mqtt-topic>] [--voltage <voltage>]\n\n", programname);
    exit(-1);
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

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
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

    libusb_context *context = NULL;
    libusb_device **list = NULL;
    static struct libusb_device_descriptor desc;
    int r;
    if ((r=libusb_init(&context)) < 0) {
        printf("ERROR: libusb_init returned %d\n", r);
        active = 0;
    }
    bool scanning = true;
    while (active) {
        if (scanning) {
            r = libusb_get_device_list(context, &list);
            if (r < 0) {
                printf("ERROR: libusb_get_device_list returned %d\n", r);
                active = 0;
            }
            int count = r;
            for (int i = 0;i<count;i++) {
                libusb_device *device = list[i];
                int seen = 0;
                for (cm160_t *cm160=head;cm160;cm160=cm160->next) {
                    if (libusb_get_device(cm160->devh) == device) {
                        seen = 1;
                        break;
                    }
                }
                if (!seen) {
                    if ((r=libusb_get_device_descriptor(device, &desc)) < 0) {
                        printf("ERROR: libusb_get_device_descriptor returned %d (%s)\n", r, libusb_strerror(r));
                    } else if (desc.idVendor == OWL_VENDOR_ID && desc.idProduct == CM160_DEV_ID) {
                        cm160_t *cm160 = calloc(sizeof(struct cm160_struct), 1);
                        if (head) {
                            cm160->next = head;
                        }
                        if ((r=libusb_open(device, &(cm160->devh))) < 0) {
                            printf("ERROR: libusb_open returned %d (%s)\n", r, libusb_strerror(r));
                            free(cm160);
                            continue;
                        }
                        if (libusb_kernel_driver_active(cm160->devh, USB_INTERFACE)) {
                            if (libusb_detach_kernel_driver(cm160->devh, 0)) {
                                printf("ERROR: libusb_detach_kernel_driver failed\n");
                            }
                            cm160->kernel = 1;
                        }
                        libusb_set_auto_detach_kernel_driver(cm160->devh, 1);
                        if ((r = libusb_set_configuration(cm160->devh, USB_CONFIGURATION))) {
                            printf("ERROR: libusb_set_configuration returned %d (%s)\n", r, libusb_strerror(r));
                        }
                        sleep(1);
                        if ((r = libusb_claim_interface(cm160->devh, USB_INTERFACE))) {
                            printf("ERROR: libusb_claim_interface returned %d (%s)\n", r, libusb_strerror(r));
                            libusb_close(cm160->devh);
                            free(cm160);
                            continue;
                        }
                        printf("CM160: connected\n");
                         // set baudrate
                        int baudrate = 250000;
                        // See https://www.silabs.com/documents/public/application-notes/AN571.pdf
                        if ((r=libusb_control_transfer(cm160->devh, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT, CP210X_IFC_ENABLE, UART_ENABLE, USB_INTERFACE, NULL, 0, 500)) < 0) {
                            printf("ERROR: libusb_control_transfer (CP210X_IFC_ENABLE on) returned %d (%s)\n", r, libusb_strerror(r));
                        }
                        if ((r=libusb_control_transfer(cm160->devh, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT, CP210X_SET_BAUDRATE, 0, USB_INTERFACE, (void *)&baudrate, sizeof(baudrate), 500)) < 0) {
                            printf("ERROR: libusb_control_transfer (CP210X_SET_BAUDRATE) returned %d (%s)\n", r, libusb_strerror(r));
                        }
//                        if ((r=libusb_control_transfer(cm160->devh, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT, CP210X_SET_LINE_CTL, 0x0800, USB_INTERFACE, NULL, 0, 500)) < 0) {
//                            printf("ERROR: libusb_control_transfer (CP210X_SET_LINE_CTL) returned %d (%s)\n", r, libusb_strerror(r));
//                        }
                        if ((r=libusb_control_transfer(cm160->devh, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT, CP210X_IFC_ENABLE, UART_DISABLE, USB_INTERFACE, NULL, 0, 500)) < 0) {
                            printf("ERROR: libusb_control_transfer (CP210X_IFC_ENABLE off) returned %d (%s)\n", r, libusb_strerror(r));
                        }
                        head = cm160;
                    }
                }
            }
            scanning = false;
        }
        for (cm160_t *cm160=head;cm160;cm160=cm160->next) {
            int n = 0;
            int disconnect = 0;
            if ((r = libusb_bulk_transfer(cm160->devh, BULK_ENDPOINT_IN, cm160->buf + cm160->buflen, sizeof(cm160->buf) - cm160->buflen, &n, 20000)) < 0) {
                printf("ERROR: libusb_bulk_transfer returned %d (%s)\n", r, libusb_strerror(r));
                disconnect = 1;
            }
            if (n >= 0) {
                cm160->buflen += n;
                // printf("read %d now %d\n", n, cm160->buflen);
                while (cm160->buflen >= FRAME_SIZE) {
                    r = process_frame(cm160);
                    if (r == 0) {
                        // We couldn't read anything - try advancing one byte
                        r = 1;
                    }
                    cm160->buflen -= r;
                    memmove(cm160->buf, cm160->buf + r, cm160->buflen);
                }
            }
            if (cm160->idcount == MAXIDCOUNT) {
                // Seems to get stuck. Disconnect, reconnect.
                // It's not hearing our replies - "ID Frame" means
                // "I haven't heard from the server for a while"
                disconnect = 2;
                printf("ERROR: stuck in ID frame loop, reconnecting\n");
            }
            if (disconnect) {
                libusb_release_interface(cm160->devh, USB_INTERFACE);
                if (cm160->kernel) {
                    libusb_attach_kernel_driver(cm160->devh, USB_INTERFACE);
                }
                libusb_close(cm160->devh);
                if (head == cm160) {
                    head = cm160->next;
                } else {
                    for (cm160_t *t=head;t;t=t->next) {
                        if (t->next == cm160) {
                            t->next = cm160->next;
                            break;
                        }
                    }
                }
                if (disconnect == 2) {
                    // Something like this seems to be needed. 
                    // stty -F /dev/ttyUSB0 ospeed 250000 ispeed 250000 cs8 raw
                    int fd = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY | O_SYNC);
                    if (fd < 0) {
                        perror("open");
                    } else {
                        struct termios tty;
                        cfmakeraw(&tty);
                        if (tcsetattr(fd, TCSANOW, &tty) != 0) {
                            perror("tcsetattr");
                        }
                        close(fd);
                    }
                }
            }
        }
        if (!head) {
            scanning = true;
            sleep(1);
        }
    }
    for (cm160_t *cm160=head;cm160;cm160=cm160->next) {
        printf("CM160: disconnecting\n");
        libusb_release_interface(cm160->devh, USB_INTERFACE);
        if (cm160->kernel) {
            libusb_attach_kernel_driver(cm160->devh, USB_INTERFACE);
        }
        libusb_close(cm160->devh);
    }
    libusb_exit(context);
    if (mosq) {
        mosquitto_destroy(mosq);
    }
    mosquitto_lib_cleanup();
    return 0;
}
