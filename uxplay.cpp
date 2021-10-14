/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <stddef.h>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <fstream>
#include <sys/utsname.h>
#include <glib-unix.h>

#include "log.h"
#include "lib/raop.h"
#include "lib/stream.h"
#include "lib/logger.h"
#include "lib/dnssd.h"
#include "renderers/video_renderer.h"
#include "renderers/audio_renderer.h"

#define VERSION "1.38"

#define DEFAULT_NAME "UxPlay"
#define DEFAULT_DEBUG_LOG false
#define LOWEST_ALLOWED_PORT 1024
#define HIGHEST_PORT 65535

static int start_server (std::vector<char> hw_addr, std::string name, unsigned short display[5],
                 unsigned short tcp[3], unsigned short udp[3], videoflip_t videoflip[2],
			 bool use_audio,  bool debug_log, std::string videosink, std::string audiosink);

static int stop_server ();

static dnssd_t *dnssd = NULL;
static raop_t *raop = NULL;
static video_renderer_t *video_renderer = NULL;
static audio_renderer_t *audio_renderer = NULL;
static logger_t *render_logger = NULL;

static bool relaunch_server = false;
static uint open_connections = 0;
static bool connections_stopped = false;
static unsigned int server_timeout = 0;
static unsigned int counter;
static bool use_video = true;

gboolean connection_callback (gpointer loop){
  if (!connections_stopped) {
        counter = 0;
    } else {
        if (++counter == server_timeout) {
	    LOGI("no connections for %d seconds: relaunch server\n",server_timeout);
	    g_main_loop_quit((GMainLoop *) loop);
        }
    }
    return TRUE;
}

static gboolean  sigint_callback(gpointer loop) {
    relaunch_server = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}

static gboolean  sigterm_callback(gpointer loop) {
    relaunch_server = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}

static void main_loop()  {
    guint connection_watch_id = 0;
    guint gst_bus_watch_id = 0;
    GMainLoop *loop = g_main_loop_new(NULL,FALSE);
    if (server_timeout) {
        connection_watch_id = g_timeout_add_seconds(1, (GSourceFunc) connection_callback, (gpointer) loop);
    }  
    if (use_video) gst_bus_watch_id = (guint) video_renderer_listen((void *)loop, video_renderer);
    guint sigterm_watch_id = g_unix_signal_add(SIGTERM, (GSourceFunc) sigterm_callback, (gpointer) loop);
    guint sigint_watch_id = g_unix_signal_add(SIGINT, (GSourceFunc) sigint_callback, (gpointer) loop);
    relaunch_server = true;
    g_main_loop_run(loop);

    if (gst_bus_watch_id > 0) g_source_remove(gst_bus_watch_id);
    if (sigint_watch_id > 0) g_source_remove(sigint_watch_id);
    if (sigterm_watch_id > 0) g_source_remove(sigterm_watch_id);
    if (connection_watch_id > 0) g_source_remove(connection_watch_id);
    g_main_loop_unref(loop);
}    

static int parse_hw_addr (std::string str, std::vector<char> &hw_addr) {
    for (int i = 0; i < str.length(); i += 3) {
        hw_addr.push_back((char) stol(str.substr(i), NULL, 16));
    }
    return 0;
}

static std::string find_mac () {
    std::ifstream iface_stream("/sys/class/net/eth0/address");
    if (!iface_stream) {
        iface_stream.open("/sys/class/net/wlan0/address");
    }
    if (!iface_stream) return "";

    std::string mac_address;
    iface_stream >> mac_address;
    iface_stream.close();
    return mac_address;
}

#define MULTICAST 0
#define LOCAL 1
#define OCTETS 6
static std::string random_mac () {
    char str[3];
    int octet = rand() % 64;
    octet = (octet << 1) + LOCAL;
    octet = (octet << 1) + MULTICAST;
    snprintf(str,3,"%02x",octet);
    std::string mac_address(str);
    for (int i = 1; i < OCTETS; i++) {
        mac_address = mac_address + ":";
        octet =  rand() % 256;
        snprintf(str,3,"%02x",octet);
        mac_address = mac_address + str;
    }
    return mac_address;
}

static void print_info (char *name) {
    printf("UxPlay %s: An open-source AirPlay mirroring server based on RPiPlay\n", VERSION);
    printf("Usage: %s [-n name] [-s wxh] [-p [n]]\n", name);
    printf("Options:\n");
    printf("-n name   Specify the network name of the AirPlay server\n");
    printf("-s wxh[@r]Set display resolution [refresh_rate] default 1920x1080[@60]\n");
    printf("-o        Set mirror \"overscanned\" mode on (not usually needed)\n");
    printf("-fps n    Set maximum allowed streaming framerate, default 30\n");
    printf("-f {H|V|I}Horizontal|Vertical flip, or both=Inversion=rotate 180 deg\n");
    printf("-r {R|L}  Rotate 90 degrees Right (cw) or Left (ccw)\n");
    printf("-p        Use legacy ports UDP 6000:6001:7011 TCP 7000:7001:7100\n");
    printf("-p n      Use TCP and UDP ports n,n+1,n+2. range %d-%d\n", LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    printf("          use \"-p n1,n2,n3\" to set each port, \"n1,n2\" for n3 = n2+1\n");
    printf("          \"-p tcp n\" or \"-p udp n\" sets TCP or UDP ports only\n");
    printf("-m        Use random MAC address (use for concurrent UxPlay's)\n");
    printf("-t n      Relaunch server if no connection existed in last n seconds\n");
    printf("-vs       Choose the GStreamer videosink; default \"autovideosink\"\n");
    printf("          choices: ximagesink,xvimagesink,vaapisink,glimagesink, etc.\n"); 
    printf("-vs 0     Streamed audio only, with no video display window\n");
    printf("-as       Choose the GStreamer audiosink; default \"autoaudiosink\"\n");
    printf("          choices: pulsesink,alsasink,osssink,oss4sink,osxaudiosink,etc.\n");
    printf("-as 0     (or -a)  Turn audio off, video output only\n");
    printf("-d        Enable debug logging\n");
    printf("-v or -h  Displays this help and version information\n");
}

bool option_has_value(const int i, const int argc, std::string option, const char *next_arg) {
    if (i >= argc - 1 || next_arg[0] == '-') {
        LOGE("invalid: \"%s\" had no argument", option.c_str());
        return false;
     }
    return true;
}

static bool get_display_settings (std::string value, unsigned short *w, unsigned short *h, unsigned short *r) {
    // assume str  = wxh@r is valid if w and h are positive decimal integers
    // with no more than 4 digits, r < 256 (stored in one byte).
    char *end;
    std::size_t pos = value.find_first_of("x");
    if (pos == std::string::npos) return false;
    std::string str1 = value.substr(pos+1);
    value.erase(pos);
    if (value.length() == 0 || value.length() > 4 || value[0] == '-') return false;
    *w = (unsigned short) strtoul(value.c_str(), &end, 10);
    if (*end || *w == 0)  return false;
    pos = str1.find_first_of("@");
    if(pos != std::string::npos) {
        std::string str2 = str1.substr(pos+1);
        if (str2.length() == 0 || str2.length() > 3 || str2[0] == '-') return false;
        *r = (unsigned short) strtoul(str2.c_str(), &end, 10);
        if (*end || *r == 0 || *r > 255) return false;
        str1.erase(pos);
    }
    if (str1.length() == 0 || str1.length() > 4 || str1[0] == '-') return false;
    *h = (unsigned short) strtoul(str1.c_str(), &end, 10);
    if (*end || *h == 0) return false;
    return true;
}

static bool get_value (const char *str, unsigned int *n) {
    // str must be a positive decimal <= input value *n  
    if (strlen(str) == 0 || strlen(str) > 10 || str[0] == '-') return false;
    char *end;
    unsigned long l = strtoul(str, &end, 10);
    if (*end || l == 0 || (*n > 0 && l > *n)) return false;
    *n = (unsigned int) l;
    return true;
}

static bool get_ports (int nports, std::string option, const char * value, unsigned short * const port) {
    /*valid entries are comma-separated values port_1,port_2,...,port_r, 0 < r <= nports */
    /*where ports are distinct, and are in the allowed range.                            */
    /*missing values are consecutive to last given value (at least one value needed).    */
    char *end;
    unsigned long l;
    std::size_t pos;
    std::string val(value), str;
    for (int i = 0; i <= nports ; i++)  {
        if(i == nports) break;
        pos = val.find_first_of(',');
        str = val.substr(0,pos);
        if(str.length() == 0 || str.length() > 5 || str[0] == '-') break;
        l = strtoul(str.c_str(), &end, 10);
        if (*end || l < LOWEST_ALLOWED_PORT || l > HIGHEST_PORT) break;
         *(port + i) = (unsigned short) l;
        for  (int j = 0; j < i ; j++) {
            if( *(port + j) == *(port + i)) break;
        }
        if(pos == std::string::npos) {
            if (nports + *(port + i) > i + 1 + HIGHEST_PORT) break;
            for (int j = i + 1; j < nports; j++) {
                *(port + j) = *(port + j - 1) + 1;
            }
            return true;
        }
        val.erase(0, pos+1);
    }
    LOGE("invalid \"%s %s\", all %d ports must be in range [%d,%d]",
         option.c_str(), value, nports, LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    return false;
}

static bool get_videoflip (const char *str, videoflip_t *videoflip) {
    if (strlen(str) > 1) return false;
    switch (str[0]) {
    case 'I':
        *videoflip = INVERT;
        break;
    case 'H':
        *videoflip = HFLIP;
        break;
    case 'V':
        *videoflip = VFLIP;
        break;
    default:
        return false;
    }
    return true;
}

static bool get_videorotate (const char *str, videoflip_t *videoflip) {
    if (strlen(str) > 1) return false;
    switch (str[0]) {
    case 'L':
        *videoflip = LEFT;
        break;
    case 'R':
        *videoflip = RIGHT;
        break;
    default:
        return false;
    }
    return true;
}

static void append_hostname(std::string &server_name) {
    struct utsname buf;
    if (!uname(&buf)) {
      std::string name = server_name;
      name.append("@");
      name.append(buf.nodename);
      server_name = name;
    }
}

int main (int argc, char *argv[]) {
    std::string server_name = DEFAULT_NAME;
    std::vector<char> server_hw_addr;
    bool use_audio = true;
    bool use_random_hw_addr = false;
    bool debug_log = DEFAULT_DEBUG_LOG;
    unsigned short display[5] = {0}, tcp[3] = {0}, udp[3] = {0};
    videoflip_t videoflip[2] = { NONE , NONE };
    std::string videosink = "autovideosink";
    std::string audiosink = "autoaudiosink";

#ifdef SUPPRESS_AVAHI_COMPAT_WARNING
    // suppress avahi_compat nag message.  avahi emits a "nag" warning (once)
    // if  getenv("AVAHI_COMPAT_NOWARN") returns null.
    static char avahi_compat_nowarn[] = "AVAHI_COMPAT_NOWARN=1";
    if (!getenv("AVAHI_COMPAT_NOWARN")) putenv(avahi_compat_nowarn);
#endif


    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "-n") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            server_name = std::string(argv[++i]);
        } else if (arg == "-s") {
            if (!option_has_value(i, argc, argv[i], argv[i+1])) exit(1);
            std::string value(argv[++i]);
            if (!get_display_settings(value, &display[0], &display[1], &display[2])) {
                fprintf(stderr, "invalid \"-s %s\"; -s wxh : max w,h=9999; -s wxh@r : max r=255\n",
                        argv[i]);
                exit(1);
            }
        } else if (arg == "-fps") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            unsigned int n = 255;
            if (!get_value(argv[++i], &n)) {
                fprintf(stderr, "invalid \"-fps %s\"; -fps n : max n=255, default n=30\n", argv[i]);
                exit(1);
            }
            display[3] = (unsigned short) n;
        } else if (arg == "-o") {
            display[4] = 1;
        } else if (arg == "-f") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            if (!get_videoflip(argv[++i], &videoflip[0])) {
                fprintf(stderr,"invalid \"-f %s\" , unknown flip type, choices are H, V, I\n",argv[i]);
                exit(1);
            }
        } else if (arg == "-r") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            if (!get_videorotate(argv[++i], &videoflip[1])) {
                fprintf(stderr,"invalid \"-r %s\" , unknown rotation  type, choices are R, L\n",argv[i]);
                exit(1);
            }
        } else if (arg == "-p") {
            if (i == argc - 1 || argv[i + 1][0] == '-') {
                tcp[0] = 7100; tcp[1] = 7000; tcp[2] = 7001;
                udp[0] = 7011; udp[1] = 6001; udp[2] = 6000;
                continue;
            }
            std::string value(argv[++i]);
            if (value == "tcp") {
                arg.append(" tcp");
                if(!get_ports(3, arg, argv[++i], tcp)) exit(1);
            } else if (value == "udp") {
                arg.append( " udp");
                if(!get_ports(3, arg, argv[++i], udp)) exit(1);
            } else {
                if(!get_ports(3, arg, argv[i], tcp)) exit(1);
                for (int j = 1; j < 3; j++) {
                    udp[j] = tcp[j];
                }
            }
        } else if (arg == "-m") {
            use_random_hw_addr  = true;
        } else if (arg == "-a") {
            use_audio = false;
        } else if (arg == "-d") {
            debug_log = !debug_log;
        } else if (arg == "-h" || arg == "-v") {
            print_info(argv[0]);
            exit(0);
        } else if (arg == "-vs") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            videosink.erase();
            videosink.append(argv[++i]);
        } else if (arg == "-as") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            audiosink.erase();
            audiosink.append(argv[++i]);
        } else if (arg == "-t") {
            if (!option_has_value(i, argc, argv[i], argv[i+1])) exit(1);
            server_timeout = 0;
            get_value(argv[++i], &server_timeout);
	} else {
            LOGE("unknown option %s, stopping\n",argv[i]);
            exit(1);
        }
    }

    if (udp[0]) LOGI("using network ports UDP %d %d %d TCP %d %d %d\n",
		     udp[0],udp[1], udp[2], tcp[0], tcp[1], tcp[2]);

    std::string mac_address;
    if (!use_random_hw_addr) mac_address = find_mac();
    if (mac_address.empty()) {
        srand(time(NULL) * getpid());
        mac_address = random_mac();
        LOGI("using randomly-generated MAC address %s\n",mac_address.c_str());
    }
    parse_hw_addr(mac_address, server_hw_addr);
    mac_address.clear();

    append_hostname(server_name);
    
    relaunch:
    connections_stopped = false;
    if (start_server(server_hw_addr, server_name, display, tcp, udp,
                     videoflip,use_audio, debug_log, videosink, audiosink)) {
        return 1;
    }

    main_loop();
    if (relaunch_server) {
            LOGI("Re-launching server...");
            stop_server();
            goto relaunch;
    } else {
        LOGI("Stopping...");
        stop_server();
    }
}

// Server callbacks
extern "C" void conn_init (void *cls) {
    open_connections++;
    connections_stopped = false;
    LOGI("Open connections: %i", open_connections);
    video_renderer_update_background(video_renderer, 1);
}

extern "C" void conn_destroy (void *cls) {
    video_renderer_update_background(video_renderer, -1);
    open_connections--;
    LOGI("Open connections: %i", open_connections);
    if(!open_connections) {
        connections_stopped = true;
    }
}

extern "C" void audio_process (void *cls, raop_ntp_t *ntp, aac_decode_struct *data) {
    if (audio_renderer != NULL) {
        audio_renderer_render_buffer(audio_renderer, ntp, data->data, data->data_len, data->pts);
    }
}

extern "C" void video_process (void *cls, raop_ntp_t *ntp, h264_decode_struct *data) {
    video_renderer_render_buffer(video_renderer, ntp, data->data, data->data_len, data->pts, data->frame_type);
}

extern "C" void audio_flush (void *cls) {
    audio_renderer_flush(audio_renderer);
}

extern "C" void video_flush (void *cls) {
    video_renderer_flush(video_renderer);
}

extern "C" void audio_set_volume (void *cls, float volume) {
    if (audio_renderer != NULL) {
        audio_renderer_set_volume(audio_renderer, volume);
    }
}

extern "C" void audio_get_format (void *cls, unsigned int audioFormat) {
    const char * audio_format;
    switch (audioFormat) {
    case 0x1000000:
        audio_format = "AAC_ELD";
        break;
    case 0x40000:
        audio_format = "ALAC";
        break;
    case 0x400000:
        audio_format = "AAC";
        break;
    case 0x0:
        audio_format = "PCM";
        break;
    default:
        audio_format = "UNKNOWN";
        break;
    }
    printf("new audio connection with audio format 0x%X %s\n", audioFormat, audio_format);
}

extern "C" void log_callback (void *cls, int level, const char *msg) {
    switch (level) {
        case LOGGER_DEBUG: {
            LOGD("%s", msg);
            break;
        }
        case LOGGER_WARNING: {
            LOGW("%s", msg);
            break;
        }
        case LOGGER_INFO: {
            LOGI("%s", msg);
            break;
        }
        case LOGGER_ERR: {
            LOGE("%s", msg);
            break;
        }
        default:
            break;
    }

}

int start_server (std::vector<char> hw_addr, std::string name, unsigned short display[5],
                 unsigned short tcp[3], unsigned short udp[3], videoflip_t videoflip[2],
		  bool use_audio, bool debug_log, std::string videosink, std::string audiosink) {
    raop_callbacks_t raop_cbs;
    memset(&raop_cbs, 0, sizeof(raop_cbs));
    raop_cbs.conn_init = conn_init;
    raop_cbs.conn_destroy = conn_destroy;
    raop_cbs.audio_process = audio_process;
    raop_cbs.video_process = video_process;
    raop_cbs.audio_flush = audio_flush;
    raop_cbs.video_flush = video_flush;
    raop_cbs.audio_set_volume = audio_set_volume;
    raop_cbs.audio_get_format = audio_get_format;
    
    raop = raop_init(10, &raop_cbs);
    if (raop == NULL) {
        LOGE("Error initializing raop!");
        return -1;
    }

    /* write desired display pixel width, pixel height, refresh_rate, max_fps, overscanned.  */
    /* use 0 for default values 1920,1080,60,30,0; these are sent to the Airplay client      */
    
    if(videosink == "0") {
        use_video = false;
        display[3] = 1; /* set fps to 1 frame per sec when no video will be shown */
    }
    if(audiosink == "0") {
        use_audio = false;
    }

    raop_set_display(raop, display[0], display[1], display[2], display[3], display[4]);

    /* network port selection (ports listed as "0" will be dynamically assigned) */
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);
    
    raop_set_log_callback(raop, log_callback, NULL);
    raop_set_log_level(raop, debug_log ? RAOP_LOG_DEBUG : LOGGER_INFO);

    render_logger = logger_init();
    if (render_logger == NULL) {
        LOGE("Could not init render_logger\n");
        stop_server();
        return -1;
    }
    logger_set_callback(render_logger, log_callback, NULL);
    logger_set_level(render_logger, debug_log ? LOGGER_DEBUG : LOGGER_INFO);

    if ((video_renderer = video_renderer_init(render_logger, name.c_str(), videoflip, videosink.c_str())) == NULL) {
        LOGE("Could not init video renderer");
        stop_server();
        return -1;
    }

    if (! use_audio) {
        LOGI("Audio disabled");
    } else if ((audio_renderer = audio_renderer_init(render_logger, video_renderer, audiosink.c_str())) ==
               NULL) {
        LOGE("Could not init audio renderer");
        stop_server();
        return -1;
    }

    if (use_video && video_renderer) video_renderer_start(video_renderer);
    if (audio_renderer) audio_renderer_start(audio_renderer);

    unsigned short port = raop_get_port(raop);
    raop_start(raop, &port);
    raop_set_port(raop, port);

    int error;
    dnssd = dnssd_init(name.c_str(), strlen(name.c_str()), hw_addr.data(), hw_addr.size(), &error);
    if (error) {
        LOGE("Could not initialize dnssd library!");
        stop_server();
        return -2;
    }

    raop_set_dnssd(raop, dnssd);

    dnssd_register_raop(dnssd, port);
    if (tcp[2]) {
        port = tcp[2];
    } else {
      port = (port != HIGHEST_PORT ? port + 1 : port - 1);
    }
    dnssd_register_airplay(dnssd, port);

    return 0;
}

int stop_server () {
    if (raop) raop_destroy(raop);
    if (dnssd) dnssd_unregister_raop(dnssd);
    if (dnssd) dnssd_unregister_airplay(dnssd);
    if (dnssd) dnssd_destroy(dnssd);
    if (audio_renderer) audio_renderer_destroy(audio_renderer);
    if (video_renderer) video_renderer_destroy(video_renderer);
    if (render_logger) logger_destroy(render_logger);
    return 0;
}
