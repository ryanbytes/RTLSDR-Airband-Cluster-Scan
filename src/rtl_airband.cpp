/*
 * RTLSDR AM/NFM demodulator, mixer, streamer and recorder
 *
 * Copyright (c) 2014 Wong Man Hang <microtony@gmail.com>
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#if defined WITH_BCM_VC && !defined __arm__
#error Broadcom VideoCore support can only be enabled on ARM builds
#endif

// From this point we may safely assume that WITH_BCM_VC implies __arm__

#ifdef WITH_BCM_VC
#include "hello_fft/gpu_fft.h"
#include "hello_fft/mailbox.h"
#else
#include <memory>
#include <vector>
#endif /* WITH_BCM_VC */

#include <fcntl.h>
#include <lame/lame.h>
#include <ogg/ogg.h>
#include <pthread.h>
#include <shout/shout.h>
#include <stdint.h>  // uint8_t
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <vorbis/vorbisenc.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <libconfig.h++>
#include <map>
#include <string>
#include "input-common.h"
#include "logging.h"
#include "rtl_airband.h"
#include "squelch.h"

#ifdef WITH_PROFILING
#include "gperftools/profiler.h"
#endif /* WITH_PROFILING */

using namespace std;
using namespace libconfig;

device_t* devices;
mixer_t* mixers;
int device_count, mixer_count;
static int devices_running = 0;
int tui = 0;  // do not display textual user interface
int shout_metadata_delay = 3;
volatile int do_exit = 0;
bool use_localtime = false;
bool multiple_demod_threads = false;
bool multiple_output_threads = false;
bool log_scan_activity = false;
bool print_cluster_scan_plan = false;
char* stats_filepath = NULL;
char* gui_status_filepath = NULL;
char* gui_last_received_filepath = NULL;
int gui_status_interval_ms = 1000;
size_t fft_size_log = DEFAULT_FFT_SIZE_LOG;
size_t fft_size = 1 << fft_size_log;
static volatile int last_received_dirty = 0;

static const int TUI_MIN_WIDTH = 79;

struct scan_tui_palette_t {
    string title = "\033[44;37m";
    string header = "\033[1;37m";
    string idle_row = "\033[0;37m";
    string open_row = "\033[1;32m";
    string edge_row = "\033[1;33m";
    string afc_row = "\033[1;36m";
    string idle_status = "\033[1;30;47m";
    string open_status = "\033[1;30;42m";
    string edge_status = "\033[1;30;43m";
    string afc_status = "\033[1;30;46m";
};

static scan_tui_palette_t scan_tui_palette;

static const char* scan_tui_status_text(char symbol) {
    switch (symbol) {
        case SIGNAL:
            return "OPEN";
        case AFC_UP:
            return "AFC+";
        case AFC_DOWN:
            return "AFC-";
        case '~':
            return "EDGE";
        default:
            return "IDLE";
    }
}

static const char* scan_tui_status_color(char symbol) {
    switch (symbol) {
        case SIGNAL:
            return scan_tui_palette.open_status.c_str();
        case AFC_UP:
        case AFC_DOWN:
            return scan_tui_palette.afc_status.c_str();
        case '~':
            return scan_tui_palette.edge_status.c_str();
        default:
            return scan_tui_palette.idle_status.c_str();
    }
}

static const char* scan_tui_row_color(char symbol) {
    switch (symbol) {
        case SIGNAL:
            return scan_tui_palette.open_row.c_str();
        case AFC_UP:
        case AFC_DOWN:
            return scan_tui_palette.afc_row.c_str();
        case '~':
            return scan_tui_palette.edge_row.c_str();
        default:
            return scan_tui_palette.idle_row.c_str();
    }
}

static bool scan_tui_valid_sgr(const char* sgr) {
    if (sgr == NULL || *sgr == '\0') {
        return false;
    }
    for (const char* p = sgr; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p) && *p != ';') {
            return false;
        }
    }
    return true;
}

static string scan_tui_escape_sgr(const char* sgr) {
    if (!scan_tui_valid_sgr(sgr)) {
        cerr << "Configuration error: scan_tui_colors entries must contain only ANSI SGR numbers and semicolons, for example \"1;37\" or \"1;30;42\"\n";
        error();
    }
    return string("\033[") + sgr + "m";
}

static void scan_tui_set_color_if_exists(Setting& colors, const char* key, string& target) {
    if (colors.exists(key)) {
        if (colors[key].getType() != Setting::TypeString) {
            cerr << "Configuration error: scan_tui_colors." << key << " must be a string\n";
            error();
        }
        target = scan_tui_escape_sgr(colors[key]);
    }
}

static void scan_tui_set_theme(const char* theme) {
    if (theme == NULL || !strcmp(theme, "bright")) {
        scan_tui_palette = scan_tui_palette_t();
    } else if (!strcmp(theme, "high_contrast")) {
        scan_tui_palette.title = "\033[1;37;40m";
        scan_tui_palette.header = "\033[1;37m";
        scan_tui_palette.idle_row = "\033[1;37m";
        scan_tui_palette.open_row = "\033[1;37;42m";
        scan_tui_palette.edge_row = "\033[1;30;43m";
        scan_tui_palette.afc_row = "\033[1;30;46m";
        scan_tui_palette.idle_status = "\033[1;37;40m";
        scan_tui_palette.open_status = "\033[1;37;42m";
        scan_tui_palette.edge_status = "\033[1;30;43m";
        scan_tui_palette.afc_status = "\033[1;30;46m";
    } else if (!strcmp(theme, "monochrome")) {
        scan_tui_palette.title = "\033[1;37;40m";
        scan_tui_palette.header = "\033[1;37m";
        scan_tui_palette.idle_row = "\033[0;37m";
        scan_tui_palette.open_row = "\033[1;37m";
        scan_tui_palette.edge_row = "\033[4;37m";
        scan_tui_palette.afc_row = "\033[7;37m";
        scan_tui_palette.idle_status = "\033[0;37m";
        scan_tui_palette.open_status = "\033[1;37m";
        scan_tui_palette.edge_status = "\033[4;37m";
        scan_tui_palette.afc_status = "\033[7;37m";
    } else if (!strcmp(theme, "deuteranopia") || !strcmp(theme, "protanopia")) {
        scan_tui_palette.title = "\033[1;37;44m";
        scan_tui_palette.header = "\033[1;37m";
        scan_tui_palette.idle_row = "\033[0;37m";
        scan_tui_palette.open_row = "\033[1;36m";
        scan_tui_palette.edge_row = "\033[1;35m";
        scan_tui_palette.afc_row = "\033[1;33m";
        scan_tui_palette.idle_status = "\033[1;30;47m";
        scan_tui_palette.open_status = "\033[1;30;46m";
        scan_tui_palette.edge_status = "\033[1;37;45m";
        scan_tui_palette.afc_status = "\033[1;30;43m";
    } else if (!strcmp(theme, "tritanopia")) {
        scan_tui_palette.title = "\033[1;37;45m";
        scan_tui_palette.header = "\033[1;37m";
        scan_tui_palette.idle_row = "\033[0;37m";
        scan_tui_palette.open_row = "\033[1;32m";
        scan_tui_palette.edge_row = "\033[1;35m";
        scan_tui_palette.afc_row = "\033[1;37m";
        scan_tui_palette.idle_status = "\033[1;30;47m";
        scan_tui_palette.open_status = "\033[1;30;42m";
        scan_tui_palette.edge_status = "\033[1;37;45m";
        scan_tui_palette.afc_status = "\033[1;37;40m";
    } else {
        cerr << "Configuration error: scan_tui_theme must be one of: bright, high_contrast, monochrome, deuteranopia, protanopia, tritanopia\n";
        error();
    }
}

static void configure_scan_tui_colors(Setting& root) {
    if (root.exists("scan_tui_theme")) {
        if (root["scan_tui_theme"].getType() != Setting::TypeString) {
            cerr << "Configuration error: scan_tui_theme must be a string\n";
            error();
        }
        scan_tui_set_theme(root["scan_tui_theme"]);
    }
    if (root.exists("scan_tui_colors")) {
        if (root["scan_tui_colors"].getType() != Setting::TypeGroup) {
            cerr << "Configuration error: scan_tui_colors must be a group\n";
            error();
        }
        Setting& colors = root["scan_tui_colors"];
        scan_tui_set_color_if_exists(colors, "title", scan_tui_palette.title);
        scan_tui_set_color_if_exists(colors, "header", scan_tui_palette.header);
        scan_tui_set_color_if_exists(colors, "idle_row", scan_tui_palette.idle_row);
        scan_tui_set_color_if_exists(colors, "open_row", scan_tui_palette.open_row);
        scan_tui_set_color_if_exists(colors, "edge_row", scan_tui_palette.edge_row);
        scan_tui_set_color_if_exists(colors, "afc_row", scan_tui_palette.afc_row);
        scan_tui_set_color_if_exists(colors, "idle_status", scan_tui_palette.idle_status);
        scan_tui_set_color_if_exists(colors, "open_status", scan_tui_palette.open_status);
        scan_tui_set_color_if_exists(colors, "edge_status", scan_tui_palette.edge_status);
        scan_tui_set_color_if_exists(colors, "afc_status", scan_tui_palette.afc_status);
    }
}

static void scan_tui_fill_line(char fill) {
    struct winsize ws;
    int width = TUI_MIN_WIDTH;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        width = ws.ws_col;
    }
    for (int i = 0; i < width; i++) {
        putchar(fill);
    }
}

static int cluster_tui_slot(device_t* dev, int channel_index) {
    if (dev->current_cluster < 0 || dev->current_cluster >= dev->cluster_count) {
        return -1;
    }
    cluster_t* cluster = dev->clusters + dev->current_cluster;
    for (int i = 0; i < cluster->channel_count; i++) {
        if (cluster->channel_indices[i] == channel_index) {
            return i;
        }
    }
    return -1;
}

static int cluster_tui_signal_count(device_t* dev) {
    if (dev->current_cluster < 0 || dev->current_cluster >= dev->cluster_count) {
        return 0;
    }
    int count = 0;
    cluster_t* cluster = dev->clusters + dev->current_cluster;
    for (int i = 0; i < cluster->channel_count; i++) {
        int channel_index = cluster->channel_indices[i];
        if (dev->channels[channel_index].axcindicate != NO_SIGNAL) {
            count++;
        }
    }
    return count;
}

static void cluster_tui_print_summary(device_t* dev, int device_num) {
    if (dev->current_cluster < 0 || dev->current_cluster >= dev->cluster_count) {
        return;
    }
    cluster_t* cluster = dev->clusters + dev->current_cluster;
    GOTOXY(0, device_num * 17 + 1);
    printf("%s", scan_tui_palette.title.c_str());
    scan_tui_fill_line(' ');
    GOTOXY(1, device_num * 17 + 1);
    printf("RTLSDR-Airband cluster_scan  dev %d  cluster %02d/%02d  channels %d/%d",
           device_num,
           dev->current_cluster + 1,
           dev->cluster_count,
           cluster->channel_count,
           dev->channel_count);
    printf("\033[0m");
    GOTOXY(0, device_num * 17 + 2);
    printf("\033[K%s center %8.3f MHz  span %8.3f-%8.3f MHz  active %d  sweep %.1fs\033[0m",
           scan_tui_palette.header.c_str(),
           cluster->center_frequency / 1000000.0,
           cluster->min_frequency / 1000000.0,
           cluster->max_frequency / 1000000.0,
           cluster_tui_signal_count(dev),
           ((dev->cluster_hang_time_ms + 100) * dev->cluster_count) / 1000.0);
    GOTOXY(0, device_num * 17 + 3);
    printf("\033[K%s%8s  %-4s  %5s  %4s  %5s  %7s  %s\033[0m", scan_tui_palette.header.c_str(), "MHz", "STATE", "CH", "SIG", "NOISE", "HITS", "LABEL");
}

#ifdef NFM
float alpha = exp(-1.0f / (WAVE_RATE * 2e-4));
enum fm_demod_algo { FM_FAST_ATAN2, FM_QUADRI_DEMOD };
enum fm_demod_algo fm_demod = FM_FAST_ATAN2;
#endif /* NFM */

#ifdef DEBUG
char* debug_path;
#endif /* DEBUG */

void sighandler(int sig) {
    log(LOG_NOTICE, "Got signal %d, exiting\n", sig);
    do_exit = 1;
}

void update_channel_tuning(device_t* dev, int channel_index) {
    channel_t* channel = dev->channels + channel_index;
    dev->base_bins[channel_index] = dev->bins[channel_index] =
        (size_t)ceil((channel->freqlist[0].frequency + dev->input->sample_rate - dev->input->centerfreq) / (double)(dev->input->sample_rate / fft_size) - 1.0) % fft_size;

#ifdef NFM
    if (channel->needs_raw_iq) {
        double dm_dphi = (double)(channel->freqlist[0].frequency - dev->input->centerfreq);
        double decimation_factor = ((double)dev->input->sample_rate / (double)WAVE_RATE);
        double dm_dphi_correction = (double)WAVE_RATE / 2.0;
        dm_dphi_correction *= (decimation_factor - round(decimation_factor));
        dm_dphi_correction *= (double)(channel->freqlist[0].frequency - dev->input->centerfreq) / ((double)dev->input->sample_rate / 2.0);
        dm_dphi -= dm_dphi_correction;
        dm_dphi /= (double)WAVE_RATE;
        dm_dphi -= trunc(dm_dphi);
        dm_dphi *= 256.0 * 65536.0;
        channel->dm_dphi = (uint32_t)((int)dm_dphi);
        channel->dm_phi = 0.f;
    }
#endif /* NFM */
}

void* controller_thread(void* params) {
    device_t* dev = (device_t*)params;
    int i = 0;
    int consecutive_squelch_off = 0;
    int new_centerfreq = 0;
    struct timeval tv;

    if (dev->channels[0].freq_count < 2)
        return 0;
    while (!do_exit) {
        SLEEP(200);
        if (dev->channels[0].axcindicate == NO_SIGNAL) {
            if (consecutive_squelch_off < 10) {
                consecutive_squelch_off++;
            } else {
                i++;
                i %= dev->channels[0].freq_count;
                dev->channels[0].freq_idx = i;
                new_centerfreq = dev->channels[0].freqlist[i].frequency + 20 * (double)(dev->input->sample_rate / fft_size);
                if (input_set_centerfreq(dev->input, new_centerfreq) < 0) {
                    break;
                }
            }
        } else {
            if (consecutive_squelch_off == 10) {
                if (log_scan_activity)
                    log(LOG_INFO, "Activity on %7.3f MHz (%s)\n", dev->channels[0].freqlist[i].frequency / 1000000.0, dev->channels[0].freqlist[i].label);
                if (i != dev->last_frequency) {
                    // squelch has just opened on a new frequency - we might need to update outputs' metadata
                    gettimeofday(&tv, NULL);
                    tag_queue_put(dev, i, tv);
                    dev->last_frequency = i;
                }
            }
            consecutive_squelch_off = 0;
        }
    }
    return 0;
}

static bool cluster_has_signal(device_t* dev) {
    if (dev->current_cluster < 0 || dev->current_cluster >= dev->cluster_count) {
        return false;
    }
    cluster_t* cluster = dev->clusters + dev->current_cluster;
    for (int i = 0; i < cluster->channel_count; i++) {
        channel_t* channel = dev->channels + cluster->channel_indices[i];
        if (channel->axcindicate != NO_SIGNAL) {
            return true;
        }
    }
    return false;
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

static void activate_cluster(device_t* dev, int cluster_index) {
    cluster_t* cluster = dev->clusters + cluster_index;
    for (int i = 0; i < dev->channel_count; i++) {
        dev->channels[i].cluster_active = false;
        dev->channels[i].axcindicate = NO_SIGNAL;
    }
    for (int i = 0; i < cluster->channel_count; i++) {
        int channel_index = cluster->channel_indices[i];
        dev->channels[channel_index].cluster_active = true;
        update_channel_tuning(dev, channel_index);
    }
    dev->current_cluster = cluster_index;
    cluster->last_scan_ms = now_ms();
}

void* cluster_controller_thread(void* params) {
    device_t* dev = (device_t*)params;
    int idle_ms = 0;

    if (dev->cluster_count < 1) {
        return 0;
    }

    activate_cluster(dev, 0);
    if (dev->cluster_count < 2) {
        return 0;
    }

    while (!do_exit) {
        SLEEP(100);
        if (cluster_has_signal(dev)) {
            idle_ms = 0;
            continue;
        }

        if (idle_ms < dev->cluster_hang_time_ms) {
            idle_ms += 100;
            continue;
        }

        int next_cluster = (dev->current_cluster + 1) % dev->cluster_count;
        int new_centerfreq = dev->clusters[next_cluster].center_frequency;
        if (input_set_centerfreq(dev->input, new_centerfreq) < 0) {
            break;
        }
        activate_cluster(dev, next_cluster);
        idle_ms = 0;
    }
    return 0;
}

void multiply(float ar, float aj, float br, float bj, float* cr, float* cj) {
    *cr = ar * br - aj * bj;
    *cj = aj * br + ar * bj;
}

#ifdef NFM
float fast_atan2(float y, float x) {
    float yabs, angle;
    float pi4 = M_PI_4, pi34 = 3 * M_PI_4;
    if (x == 0.0f && y == 0.0f) {
        return 0;
    }
    yabs = y;
    if (yabs < 0.0f) {
        yabs = -yabs;
    }
    if (x >= 0.0f) {
        angle = pi4 - pi4 * (x - yabs) / (x + yabs);
    } else {
        angle = pi34 - pi4 * (x + yabs) / (yabs - x);
    }
    if (y < 0.0f) {
        return -angle;
    }
    return angle;
}

float polar_disc_fast(float ar, float aj, float br, float bj) {
    float cr, cj;
    multiply(ar, aj, br, -bj, &cr, &cj);
    return (float)(fast_atan2(cj, cr) * M_1_PI);
}

float fm_quadri_demod(float ar, float aj, float br, float bj) {
    return (float)((br * aj - ar * bj) / (ar * ar + aj * aj + 1.0f) * M_1_PI);
}

#endif /* NFM */

class AFC {
    const status _prev_axcindicate;

#ifdef WITH_BCM_VC
    float square(const GPU_FFT_COMPLEX* fft_results, size_t index) {
        return fft_results[index].re * fft_results[index].re + fft_results[index].im * fft_results[index].im;
    }
#else
    float square(const fftwf_complex* fft_results, size_t index) {
        return fft_results[index][0] * fft_results[index][0] + fft_results[index][1] * fft_results[index][1];
    }
#endif /* WITH_BCM_VC */

    template <class FFT_RESULTS, int STEP>
    size_t check(const FFT_RESULTS* fft_results, const size_t base, const float base_value, unsigned char afc) {
        float threshold = 0;
        size_t bin;
        for (bin = base;; bin += STEP) {
            if (STEP < 0) {
                if (bin < -STEP)
                    break;

            } else if ((size_t)(bin + STEP) >= fft_size)
                break;

            const float value = square(fft_results, (size_t)(bin + STEP));
            if (value <= base_value)
                break;

            if (base == (size_t)bin) {
                threshold = (value - base_value) / (float)afc;
            } else {
                if ((value - base_value) < threshold)
                    break;

                threshold += threshold / 10.0;
            }
        }
        return bin;
    }

   public:
    AFC(device_t* dev, int index) : _prev_axcindicate(dev->channels[index].axcindicate) {}

    template <class FFT_RESULTS>
    void finalize(device_t* dev, int index, const FFT_RESULTS* fft_results) {
        channel_t* channel = &dev->channels[index];
        if (channel->afc == 0)
            return;

        const char axcindicate = channel->axcindicate;
        if (axcindicate != NO_SIGNAL && _prev_axcindicate == NO_SIGNAL) {
            const size_t base = dev->base_bins[index];
            const float base_value = square(fft_results, base);
            size_t bin = check<FFT_RESULTS, -1>(fft_results, base, base_value, channel->afc);
            if (bin == base)
                bin = check<FFT_RESULTS, 1>(fft_results, base, base_value, channel->afc);

            if (dev->bins[index] != bin) {
#ifdef AFC_LOGGING
                log(LOG_INFO, "AFC device=%d channel=%d: base=%zu prev=%zu now=%zu\n", dev->device, index, base, dev->bins[index], bin);
#endif /* AFC_LOGGING */
                dev->bins[index] = bin;
                if (bin > base)
                    channel->axcindicate = AFC_UP;
                else if (bin < base)
                    channel->axcindicate = AFC_DOWN;
            }
        } else if (axcindicate == NO_SIGNAL && _prev_axcindicate != NO_SIGNAL)
            dev->bins[index] = dev->base_bins[index];
    }
};

void init_demod(demod_params_t* params, Signal* signal, int device_start, int device_end) {
    assert(params != NULL);
    assert(signal != NULL);

    params->mp3_signal = signal;
    params->device_start = device_start;
    params->device_end = device_end;

#ifndef WITH_BCM_VC
    params->fftin = fftwf_alloc_complex(fft_size);
    params->fftout = fftwf_alloc_complex(fft_size);
    params->fft = fftwf_plan_dft_1d(fft_size, params->fftin, params->fftout, FFTW_FORWARD, FFTW_MEASURE);
#endif /* WITH_BCM_VC */
}

bool init_output(channel_t* channel, output_t* output) {
    if (output->has_mp3_output) {
        output->lame = airlame_init(channel->mode, channel->highpass, channel->lowpass);
        output->lamebuf = (unsigned char*)malloc(sizeof(unsigned char) * LAMEBUF_SIZE);
    }
    if (output->type == O_ICECAST) {
        shout_setup((icecast_data*)(output->data), channel->mode);
    } else if (output->type == O_UDP_STREAM) {
        udp_stream_data* sdata = (udp_stream_data*)(output->data);
        if (!udp_stream_init(sdata, channel->mode, (size_t)WAVE_BATCH * sizeof(float))) {
            return false;
        }
#ifdef WITH_PULSEAUDIO
    } else if (output->type == O_PULSE) {
        pulse_init();
        pulse_setup((pulse_data*)(output->data), channel->mode);
#endif /* WITH_PULSEAUDIO */
    }

    return true;
}

void init_output_params(output_params_t* params, int device_start, int device_end, int mixer_start, int mixer_end) {
    assert(params != NULL);

    params->mp3_signal = new Signal;
    params->device_start = device_start;
    params->device_end = device_end;
    params->mixer_start = mixer_start;
    params->mixer_end = mixer_end;
}

int next_device(demod_params_t* params, int current) {
    current++;
    if (current < params->device_end) {
        return current;
    }
    return params->device_start;
}

void* demodulate(void* params) {
    assert(params != NULL);
    demod_params_t* demod_params = (demod_params_t*)params;

    debug_print("Starting demod thread, devices %d:%d, signal %p\n", demod_params->device_start, demod_params->device_end, demod_params->mp3_signal);

    // initialize fft engine
#ifdef WITH_BCM_VC
    int mb = mbox_open();
    struct GPU_FFT* fft;
    int ret = gpu_fft_prepare(mb, fft_size_log, GPU_FFT_FWD, FFT_BATCH, &fft);
    switch (ret) {
        case -1:
            log(LOG_CRIT, "Unable to enable V3D. Please check your firmware is up to date.\n");
            error();
            break;
        case -2:
            log(LOG_CRIT, "log2_N=%d not supported. Try between 8 and 17.\n", fft_size_log);
            error();
            break;
        case -3:
            log(LOG_CRIT, "Out of memory. Try a smaller batch or increase GPU memory.\n");
            error();
            break;
    }
#else
    fftwf_complex* fftin = demod_params->fftin;
    fftwf_complex* fftout = demod_params->fftout;
#endif /* WITH_BCM_VC */

    float ALIGNED32 levels_u8[256], levels_s8[256];
    float* levels_ptr = NULL;

    for (int i = 0; i < 256; i++) {
        levels_u8[i] = (i - 127.5f) / 127.5f;
    }
    for (int16_t i = -127; i < 128; i++) {
        levels_s8[(uint8_t)i] = i / 128.0f;
    }

    // initialize fft window
    // blackman 7
    // the whole matrix is computed
#ifdef WITH_BCM_VC
    // TODO: change this to std::vector<float> ?
    float ALIGNED32 window[fft_size * 2];
#else
    std::unique_ptr<float[], decltype(&fftwf_free)> window(fftwf_alloc_real(fft_size), fftwf_free);
#endif /* WITH_BCM_VC */

    const double a0 = 0.27105140069342f;
    const double a1 = 0.43329793923448f;
    const double a2 = 0.21812299954311f;
    const double a3 = 0.06592544638803f;
    const double a4 = 0.01081174209837f;
    const double a5 = 0.00077658482522f;
    const double a6 = 0.00001388721735f;

    for (size_t i = 0; i < fft_size; i++) {
        double x = a0 - (a1 * cos((2.0 * M_PI * i) / (fft_size - 1))) + (a2 * cos((4.0 * M_PI * i) / (fft_size - 1))) - (a3 * cos((6.0 * M_PI * i) / (fft_size - 1))) +
                   (a4 * cos((8.0 * M_PI * i) / (fft_size - 1))) - (a5 * cos((10.0 * M_PI * i) / (fft_size - 1))) + (a6 * cos((12.0 * M_PI * i) / (fft_size - 1)));
#ifdef WITH_BCM_VC
        window[i * 2] = window[i * 2 + 1] = (float)x;
#else
        window[i] = (float)x;
#endif /* WITH_BCM_VC */
    }

#ifdef DEBUG
    struct timeval ts, te;
    gettimeofday(&ts, NULL);
#endif /* DEBUG */
    size_t available;
    int device_num = demod_params->device_start;
    while (true) {
        if (do_exit) {
#ifdef WITH_BCM_VC
            log(LOG_INFO, "Freeing GPU memory\n");
            gpu_fft_release(fft);
#endif /* WITH_BCM_VC */
            return NULL;
        }

        device_t* dev = devices + device_num;

        pthread_mutex_lock(&dev->input->buffer_lock);
        if (dev->input->bufe >= dev->input->bufs)
            available = dev->input->bufe - dev->input->bufs;
        else
            available = dev->input->buf_size - dev->input->bufs + dev->input->bufe;
        pthread_mutex_unlock(&dev->input->buffer_lock);

        if (devices_running == 0) {
            log(LOG_ERR, "All receivers failed, exiting\n");
            do_exit = 1;
            continue;
        }

        if (dev->input->state != INPUT_RUNNING) {
            if (dev->input->state == INPUT_FAILED) {
                dev->input->state = INPUT_DISABLED;
                devices_running--;
            }
            device_num = next_device(demod_params, device_num);
            continue;
        }

        // number of input bytes per output wave sample (x 2 for I and Q)
        size_t bps = 2 * dev->input->bytes_per_sample * (size_t)round((double)dev->input->sample_rate / (double)WAVE_RATE);
        if (available < bps * FFT_BATCH + fft_size * dev->input->bytes_per_sample * 2) {
            // move to next device
            device_num = next_device(demod_params, device_num);
            SLEEP(10);
            continue;
        }

        if (dev->input->sfmt == SFMT_S16) {
            float const scale = 1.0f / dev->input->fullscale;
#ifdef WITH_BCM_VC
            struct GPU_FFT_COMPLEX* ptr = fft->in;
            for (size_t b = 0; b < FFT_BATCH; b++, ptr += fft->step) {
                short* buf2 = (short*)(dev->input->buffer + dev->input->bufs + b * bps);
                for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                    ptr[i].re = scale * (float)buf2[0] * window[i * 2];
                    ptr[i].im = scale * (float)buf2[1] * window[i * 2];
                }
            }
#else
            short* buf2 = (short*)(dev->input->buffer + dev->input->bufs);
            for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                fftin[i][0] = scale * (float)buf2[0] * window[i];
                fftin[i][1] = scale * (float)buf2[1] * window[i];
            }
#endif /* WITH_BCM_VC */
        } else if (dev->input->sfmt == SFMT_F32) {
            float const scale = 1.0f / dev->input->fullscale;
#ifdef WITH_BCM_VC
            struct GPU_FFT_COMPLEX* ptr = fft->in;
            for (size_t b = 0; b < FFT_BATCH; b++, ptr += fft->step) {
                float* buf2 = (float*)(dev->input->buffer + dev->input->bufs + b * bps);
                for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                    ptr[i].re = scale * buf2[0] * window[i * 2];
                    ptr[i].im = scale * buf2[1] * window[i * 2];
                }
            }
#else  // WITH_BCM_VC
            float* buf2 = (float*)(dev->input->buffer + dev->input->bufs);
            for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                fftin[i][0] = scale * buf2[0] * window[i];
                fftin[i][1] = scale * buf2[1] * window[i];
            }
#endif /* WITH_BCM_VC */

        } else {  // S8 or U8
            levels_ptr = (dev->input->sfmt == SFMT_U8 ? levels_u8 : levels_s8);

#ifdef WITH_BCM_VC
            sample_fft_arg sfa = {fft_size / 4, fft->in};
            for (size_t i = 0; i < FFT_BATCH; i++) {
                samplefft(&sfa, dev->input->buffer + dev->input->bufs + i * bps, window, levels_ptr);
                sfa.dest += fft->step;
            }
#else
            unsigned char* buf2 = dev->input->buffer + dev->input->bufs;
            for (size_t i = 0; i < fft_size; i++, buf2 += 2) {
                fftin[i][0] = levels_ptr[buf2[0]] * window[i];
                fftin[i][1] = levels_ptr[buf2[1]] * window[i];
            }
#endif /* WITH_BCM_VC */
        }

#ifdef WITH_BCM_VC
        gpu_fft_execute(fft);
#else
        fftwf_execute(demod_params->fft);
#endif /* WITH_BCM_VC */

#ifdef WITH_BCM_VC
        for (int i = 0; i < dev->channel_count; i++) {
            float* wavein = dev->channels[i].wavein + dev->waveend;
            __builtin_prefetch(wavein, 1);
            const int bin = dev->bins[i];
            const GPU_FFT_COMPLEX* fftout = fft->out + bin;
            for (int j = 0; j < FFT_BATCH; j++, ++wavein, fftout += fft->step)
                *wavein = sqrtf(fftout->im * fftout->im + fftout->re * fftout->re);
        }
        for (int j = 0; j < dev->channel_count; j++) {
            if (dev->channels[j].needs_raw_iq) {
                struct GPU_FFT_COMPLEX* ptr = fft->out;
                for (int job = 0; job < FFT_BATCH; job++) {
                    dev->channels[j].iq_in[2 * (dev->waveend + job)] = ptr[dev->bins[j]].re;
                    dev->channels[j].iq_in[2 * (dev->waveend + job) + 1] = ptr[dev->bins[j]].im;
                    ptr += fft->step;
                }
            }
        }
#else
        for (int j = 0; j < dev->channel_count; j++) {
            dev->channels[j].wavein[dev->waveend] = sqrtf(fftout[dev->bins[j]][0] * fftout[dev->bins[j]][0] + fftout[dev->bins[j]][1] * fftout[dev->bins[j]][1]);
            if (dev->channels[j].needs_raw_iq) {
                dev->channels[j].iq_in[2 * dev->waveend] = fftout[dev->bins[j]][0];
                dev->channels[j].iq_in[2 * dev->waveend + 1] = fftout[dev->bins[j]][1];
            }
        }
#endif /* WITH_BCM_VC */

        dev->waveend += FFT_BATCH;

        if (dev->waveend >= WAVE_BATCH + AGC_EXTRA) {
            for (int i = 0; i < dev->channel_count; i++) {
                AFC afc(dev, i);
                channel_t* channel = dev->channels + i;
                freq_t* fparms = channel->freqlist + channel->freq_idx;

                // set to NO_SIGNAL, will be updated to SIGNAL based on squelch below
                channel->axcindicate = NO_SIGNAL;

                if (dev->mode == R_CLUSTER_SCAN && !channel->cluster_active) {
                    memset(channel->waveout, 0, WAVE_BATCH * sizeof(float));
                    memmove(channel->wavein, channel->wavein + WAVE_BATCH, (dev->waveend - WAVE_BATCH) * sizeof(float));
                    if (channel->needs_raw_iq) {
                        memmove(channel->iq_in, channel->iq_in + 2 * WAVE_BATCH, (dev->waveend - WAVE_BATCH) * sizeof(float) * 2);
                    }
                    continue;
                }

                for (int j = AGC_EXTRA; j < WAVE_BATCH + AGC_EXTRA; j++) {
                    float& real = channel->iq_in[2 * (j - AGC_EXTRA)];
                    float& imag = channel->iq_in[2 * (j - AGC_EXTRA) + 1];

                    fparms->squelch.process_raw_sample(channel->wavein[j]);

                    // If squelch is open / opening and using I/Q, then cleanup the signal and possibly update squelch.
                    if (fparms->squelch.should_filter_sample() && channel->needs_raw_iq) {
                        // remove phase rotation introduced by FFT sliding window
                        float swf, cwf, re_tmp, im_tmp;
                        sincosf_lut(channel->dm_phi, &swf, &cwf);
                        multiply(real, imag, cwf, -swf, &re_tmp, &im_tmp);
                        channel->dm_phi += channel->dm_dphi;
                        channel->dm_phi &= 0xffffff;

                        // apply lowpass filter, will be a no-op if not configured
                        fparms->lowpass_filter.apply(re_tmp, im_tmp);

                        // update I/Q and wave
                        real = re_tmp;
                        imag = im_tmp;
                        channel->wavein[j] = sqrt(real * real + imag * imag);

                        // update squelch post-cleanup
                        if (fparms->lowpass_filter.enabled()) {
                            fparms->squelch.process_filtered_sample(channel->wavein[j]);
                        }
                    }

                    if (fparms->modulation == MOD_AM) {
                        // if squelch is just opening then bootstrip agcavgfast with prior values of wavein
                        if (fparms->squelch.first_open_sample()) {
                            for (int k = j - AGC_EXTRA; k < j; k++) {
                                if (channel->wavein[k] >= fparms->squelch.squelch_level()) {
                                    fparms->agcavgfast = fparms->agcavgfast * 0.9f + channel->wavein[k] * 0.1f;
                                }
                            }
                        }
                        // if squelch is just closing then fade out the prior samples of waveout
                        else if (fparms->squelch.last_open_sample()) {
                            for (int k = j - AGC_EXTRA + 1; k < j; k++) {
                                channel->waveout[k] = channel->waveout[k - 1] * 0.94f;
                            }
                        }
                    }

                    float& waveout = channel->waveout[j];

                    // If squelch sees power then do modulation-specific processing
                    if (fparms->squelch.should_process_audio()) {
                        if (fparms->modulation == MOD_AM) {
                            if (channel->wavein[j] > fparms->squelch.squelch_level()) {
                                fparms->agcavgfast = fparms->agcavgfast * 0.995f + channel->wavein[j] * 0.005f;
                            }

                            waveout = (channel->wavein[j - AGC_EXTRA] - fparms->agcavgfast) / (fparms->agcavgfast * 1.5f);
                            if (abs(waveout) > 0.8f) {
                                waveout *= 0.85f;
                                fparms->agcavgfast *= 1.15f;
                            }
                        }
#ifdef NFM
                        else if (fparms->modulation == MOD_NFM) {
                            // FM demod
                            if (fm_demod == FM_FAST_ATAN2) {
                                waveout = polar_disc_fast(real, imag, channel->pr, channel->pj);
                            } else if (fm_demod == FM_QUADRI_DEMOD) {
                                waveout = fm_quadri_demod(real, imag, channel->pr, channel->pj);
                            }
                            channel->pr = real;
                            channel->pj = imag;

                            // de-emphasis IIR + DC blocking
                            fparms->agcavgfast = fparms->agcavgfast * 0.995f + waveout * 0.005f;
                            waveout -= fparms->agcavgfast;
                            waveout = waveout * (1.0f - channel->alpha) + channel->prev_waveout * channel->alpha;

                            // save off waveout before notch and ampfactor
                            channel->prev_waveout = waveout;
                        }
#endif /* NFM */

                        // process audio sample for CTCSS, will be no-op if not configured
                        fparms->squelch.process_audio_sample(waveout);
                    }

                    // If squelch is still open then save samples to output
                    if (fparms->squelch.is_open()) {
                        // apply the notch filter, will be a no-op if not configured
                        fparms->notch_filter.apply(waveout);

                        // apply the ampfactor
                        waveout *= fparms->ampfactor;

                        // make sure the value is between +/- 1 (requirement for libmp3lame)
                        if (isnan(waveout)) {
                            waveout = 0.0;
                        } else if (waveout > 1.0) {
                            waveout = 1.0;
                        } else if (waveout < -1.0) {
                            waveout = -1.0;
                        }

                        channel->axcindicate = SIGNAL;
                        if (channel->has_iq_outputs) {
                            channel->iq_out[2 * (j - AGC_EXTRA)] = real;
                            channel->iq_out[2 * (j - AGC_EXTRA) + 1] = imag;
                        }

                        // Squelch is closed
                    } else {
                        waveout = 0;
                        if (channel->has_iq_outputs) {
                            channel->iq_out[2 * (j - AGC_EXTRA)] = 0;
                            channel->iq_out[2 * (j - AGC_EXTRA) + 1] = 0;
                        }
                    }
                }
                memmove(channel->wavein, channel->wavein + WAVE_BATCH, (dev->waveend - WAVE_BATCH) * sizeof(float));
                if (channel->needs_raw_iq) {
                    memmove(channel->iq_in, channel->iq_in + 2 * WAVE_BATCH, (dev->waveend - WAVE_BATCH) * sizeof(float) * 2);
                }

#ifdef WITH_BCM_VC
                afc.finalize(dev, i, fft->out);
#else
                afc.finalize(dev, i, demod_params->fftout);
#endif /* WITH_BCM_VC */

                if (tui) {
                    char symbol = fparms->squelch.signal_outside_filter() ? '~' : (char)channel->axcindicate;
                    if (dev->mode == R_SCAN) {
                        GOTOXY(0, device_num * 17 + dev->row + 3);
                        const char* label = fparms->label == NULL ? "" : fparms->label;
                        printf("\033[K%s%8.3f  \033[0m%s%-4s\033[0m%s  %02d/%02d  %4.0f  %5.0f  %7zu  %-48.48s\033[0m",
                               scan_tui_status_color(symbol),
                               fparms->frequency / 1000000.0,
                               scan_tui_status_color(symbol),
                               scan_tui_status_text(symbol),
                               scan_tui_row_color(symbol),
                               channel->freq_idx + 1,
                               channel->freq_count,
                               level_to_dBFS(fparms->squelch.signal_level()),
                               level_to_dBFS(fparms->squelch.noise_level()),
                               fparms->active_counter,
                               label);
                    } else if (dev->mode == R_CLUSTER_SCAN) {
                        int slot = cluster_tui_slot(dev, i);
                        if (slot >= 0 && slot < 11) {
                            cluster_tui_print_summary(dev, device_num);
                            if (slot == 0) {
                                for (int row = 4; row < 16; row++) {
                                    GOTOXY(0, device_num * 17 + row);
                                    printf("\033[K");
                                }
                            }
                            GOTOXY(0, device_num * 17 + slot + 4);
                            const char* label = fparms->label == NULL ? "" : fparms->label;
                            printf("\033[K%s%8.3f  \033[0m%s%-4s\033[0m%s  %5d  %4.0f  %5.0f  %7zu  %-48.48s\033[0m",
                                   scan_tui_status_color(symbol),
                                   fparms->frequency / 1000000.0,
                                   scan_tui_status_color(symbol),
                                   scan_tui_status_text(symbol),
                                   scan_tui_row_color(symbol),
                                   i + 1,
                                   level_to_dBFS(fparms->squelch.signal_level()),
                                   level_to_dBFS(fparms->squelch.noise_level()),
                                   fparms->active_counter,
                                   label);
                        }
                    } else {
                        GOTOXY(i * 10, device_num * 17 + dev->row + 3);
                        printf("%4.0f/%3.0f%c ", level_to_dBFS(fparms->squelch.signal_level()), level_to_dBFS(fparms->squelch.noise_level()), symbol);
                    }
                    fflush(stdout);
                }

                if (channel->axcindicate != NO_SIGNAL) {
                    freq_t* active_freq = channel->freqlist + channel->freq_idx;
                    active_freq->active_counter++;
                    active_freq->last_received_ms = now_ms();
                    last_received_dirty = 1;
                }
            }
            if (dev->waveavail == 1) {
                debug_print("devices[%d]: output channel overrun\n", device_num);
                dev->output_overrun_count++;
            } else {
                dev->waveavail = 1;
            }
            dev->waveend -= WAVE_BATCH;
#ifdef DEBUG
            gettimeofday(&te, NULL);
            debug_bulk_print("waveavail %lu.%lu %lu\n", te.tv_sec, (unsigned long)te.tv_usec, (te.tv_sec - ts.tv_sec) * 1000000UL + te.tv_usec - ts.tv_usec);
            ts.tv_sec = te.tv_sec;
            ts.tv_usec = te.tv_usec;
#endif /* DEBUG */
            demod_params->mp3_signal->send();
            dev->row++;
            if (dev->row == 12) {
                dev->row = 0;
            }
        }

        dev->input->bufs = (dev->input->bufs + bps * FFT_BATCH) % dev->input->buf_size;
        device_num = next_device(demod_params, device_num);
    }
}

void usage() {
    cout << "Usage: rtl_airband [options] [-c <config_file_path>]\n\
\t-h\t\t\tDisplay this help text\n\
\t-f\t\t\tRun in foreground, display textual waterfalls\n\
\t-F\t\t\tRun in foreground, do not display waterfalls (for running as a systemd service)\n";
#ifdef NFM
    cout << "\t-Q\t\t\tUse quadri correlator for FM demodulation (default is atan2)\n";
#endif /* NFM */
#ifdef DEBUG
    cout << "\t-d <file>\t\tLog debugging information to <file> (default is " << DEBUG_PATH << ")\n";
#endif /* DEBUG */
    cout << "\t-e\t\t\tPrint messages to standard error (disables syslog logging)\n";
    cout << "\t-c <config_file_path>\tUse non-default configuration file\n\t\t\t\t(default: " << CFGFILE << ")\n\
\t-P\t\t\tPrint clustered scan plan and exit without starting SDR devices\n\
\t-v\t\t\tDisplay version and exit\n";
    exit(EXIT_SUCCESS);
}

static int count_devices_running() {
    int ret = 0;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].input->state == INPUT_RUNNING) {
            ret++;
        }
    }
    return ret;
}

static void print_cluster_plan_stdout() {
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        if (dev->mode != R_CLUSTER_SCAN) {
            continue;
        }
        cout << "Device #" << i << " cluster_scan plan\n";
        cout << "  programmed frequencies: " << dev->channel_count << "\n";
        cout << "  sample_rate: " << dev->input->sample_rate << " Hz\n";
        cout << "  edge_guard_hz: " << dev->edge_guard_hz << "\n";
        cout << "  usable_bandwidth: " << dev->usable_bandwidth_hz << " Hz\n";
        cout << "  clusters: " << dev->cluster_count << "\n";
        cout << "  estimated full sweep time: " << ((dev->cluster_hang_time_ms + 100) * dev->cluster_count) / 1000.0 << " seconds\n";
        for (int c = 0; c < dev->cluster_count; c++) {
            cluster_t* cluster = dev->clusters + c;
            cout << "  cluster " << c << ": center " << cluster->center_frequency << " Hz, channels";
            for (int j = 0; j < cluster->channel_count; j++) {
                int channel_index = cluster->channel_indices[j];
                cout << " " << dev->channels[channel_index].freqlist[0].frequency;
            }
            cout << "\n";
        }
    }
}

static void log_cluster_scan_plans() {
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        if (dev->mode != R_CLUSTER_SCAN) {
            continue;
        }
        log(LOG_INFO, "Device #%d cluster_scan: %d programmed frequencies, sample_rate %d Hz, usable bandwidth %d Hz, %d clusters, estimated full sweep time %.2f seconds\n",
            i, dev->channel_count, dev->input->sample_rate, dev->usable_bandwidth_hz, dev->cluster_count, ((dev->cluster_hang_time_ms + 100) * dev->cluster_count) / 1000.0);
        for (int c = 0; c < dev->cluster_count; c++) {
            cluster_t* cluster = dev->clusters + c;
            log(LOG_INFO, "Device #%d cluster_scan cluster %d: center %.6f MHz, %d channels, span %.6f-%.6f MHz\n",
                i, c, cluster->center_frequency / 1000000.0, cluster->channel_count, cluster->min_frequency / 1000000.0, cluster->max_frequency / 1000000.0);
            for (int j = 0; j < cluster->channel_count; j++) {
                int channel_index = cluster->channel_indices[j];
                log(LOG_INFO, "Device #%d cluster_scan cluster %d channel %d: %.6f MHz\n",
                    i, c, channel_index, dev->channels[channel_index].freqlist[0].frequency / 1000000.0);
            }
        }
    }
}

static void json_write_string(FILE* fp, const char* value) {
    fputc('"', fp);
    if (value != NULL) {
        for (const unsigned char* p = (const unsigned char*)value; *p != '\0'; p++) {
            if (*p == '"' || *p == '\\') {
                fputc('\\', fp);
                fputc(*p, fp);
            } else if (*p >= 0x20) {
                fputc(*p, fp);
            }
        }
    }
    fputc('"', fp);
}

static void load_last_received_state(void) {
    if (gui_last_received_filepath == NULL) {
        return;
    }

    FILE* fp = fopen(gui_last_received_filepath, "r");
    if (fp == NULL) {
        if (errno != ENOENT) {
            log(LOG_WARNING, "Could not read last received state file %s: %s\n", gui_last_received_filepath, strerror(errno));
        }
        return;
    }

    map<int, long long> timestamps;
    int frequency = 0;
    long long timestamp = 0;
    while (fscanf(fp, "%d %lld", &frequency, &timestamp) == 2) {
        if (frequency > 0 && timestamp > 0) {
            timestamps[frequency] = timestamp;
        }
    }
    fclose(fp);

    int restored = 0;
    for (int d = 0; d < device_count; d++) {
        device_t* dev = devices + d;
        for (int c = 0; c < dev->channel_count; c++) {
            channel_t* channel = dev->channels + c;
            for (int f = 0; f < channel->freq_count; f++) {
                freq_t* freq = channel->freqlist + f;
                map<int, long long>::const_iterator it = timestamps.find(freq->frequency);
                if (it != timestamps.end()) {
                    freq->last_received_ms = it->second;
                    restored++;
                }
            }
        }
    }
    log(LOG_INFO, "Loaded last received timestamps for %d configured frequencies from %s\n", restored, gui_last_received_filepath);
}

static void save_last_received_state(void) {
    if (gui_last_received_filepath == NULL) {
        return;
    }

    string tmp_path = string(gui_last_received_filepath) + ".tmp";
    FILE* fp = fopen(tmp_path.c_str(), "w");
    if (fp == NULL) {
        log(LOG_WARNING, "Could not write last received state file %s: %s\n", tmp_path.c_str(), strerror(errno));
        return;
    }

    map<int, long long> timestamps;
    for (int d = 0; d < device_count; d++) {
        device_t* dev = devices + d;
        for (int c = 0; c < dev->channel_count; c++) {
            channel_t* channel = dev->channels + c;
            for (int f = 0; f < channel->freq_count; f++) {
                freq_t* freq = channel->freqlist + f;
                if (freq->last_received_ms > 0) {
                    timestamps[freq->frequency] = freq->last_received_ms;
                }
            }
        }
    }

    for (map<int, long long>::const_iterator it = timestamps.begin(); it != timestamps.end(); ++it) {
        fprintf(fp, "%d %lld\n", it->first, it->second);
    }
    fclose(fp);
    if (rename(tmp_path.c_str(), gui_last_received_filepath) != 0) {
        log(LOG_WARNING, "Could not replace last received state file %s: %s\n", gui_last_received_filepath, strerror(errno));
    }
}

static const char* mode_name(rec_modes mode) {
    switch (mode) {
        case R_SCAN:
            return "scan";
        case R_CLUSTER_SCAN:
            return "cluster_scan";
        default:
            return "multichannel";
    }
}

static void write_gui_status(void) {
    if (gui_status_filepath == NULL) {
        return;
    }
    string tmp_path = string(gui_status_filepath) + ".tmp";
    FILE* fp = fopen(tmp_path.c_str(), "w");
    if (fp == NULL) {
        log(LOG_WARNING, "Could not write GUI status file %s: %s\n", tmp_path.c_str(), strerror(errno));
        return;
    }
    fprintf(fp, "{\n  \"timestamp_ms\": %lld,\n  \"devices\": [\n", now_ms());
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(fp, "%s    {\n      \"index\": %d,\n      \"mode\": ", i == 0 ? "" : ",\n", i);
        json_write_string(fp, mode_name(dev->mode));
        fprintf(fp, ",\n      \"channel_count\": %d,\n      \"input_overflows\": %zu,\n      \"output_overruns\": %zu", dev->channel_count, dev->input->overflow_count, dev->output_overrun_count);
        if (dev->mode == R_CLUSTER_SCAN && dev->cluster_count > 0) {
            cluster_t* cluster = dev->clusters + dev->current_cluster;
            fprintf(fp,
                    ",\n      \"cluster_count\": %d,\n      \"current_cluster\": %d,\n      \"center_frequency\": %d,\n      \"min_frequency\": %d,\n      \"max_frequency\": %d,\n      \"clusters\": [",
                    dev->cluster_count,
                    dev->current_cluster,
                    cluster->center_frequency,
                    cluster->min_frequency,
                    cluster->max_frequency);
            for (int c = 0; c < dev->cluster_count; c++) {
                cluster_t* status_cluster = dev->clusters + c;
                fprintf(fp,
                        "%s\n        {\"index\": %d, \"center_frequency\": %d, \"min_frequency\": %d, \"max_frequency\": %d, \"last_scan_ms\": %lld, \"channels\": [",
                        c == 0 ? "" : ",",
                        c,
                        status_cluster->center_frequency,
                        status_cluster->min_frequency,
                        status_cluster->max_frequency,
                        status_cluster->last_scan_ms);
                for (int j = 0; j < status_cluster->channel_count; j++) {
                    int channel_index = status_cluster->channel_indices[j];
                    channel_t* channel = dev->channels + channel_index;
                    freq_t* freq = channel->freqlist;
                    fprintf(fp, "%s{\"index\": %d, \"frequency\": %d, \"label\": ", j == 0 ? "" : ", ", channel_index, freq->frequency);
                    json_write_string(fp, freq->label);
                    fprintf(fp, ", \"state\": ");
                    json_write_string(fp, channel->axcindicate == NO_SIGNAL ? "idle" : "open");
                    fprintf(fp, ", \"hits\": %zu, \"last_received_ms\": %lld}", freq->active_counter, freq->last_received_ms);
                }
                fprintf(fp, "]}");
            }
            fprintf(fp, "\n      ]");
        }
        fprintf(fp, "\n    }");
    }
    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
    if (rename(tmp_path.c_str(), gui_status_filepath) != 0) {
        log(LOG_WARNING, "Could not replace GUI status file %s: %s\n", gui_status_filepath, strerror(errno));
    }
}

static void* gui_status_thread(void*) {
    while (!do_exit) {
        write_gui_status();
        if (last_received_dirty) {
            save_last_received_state();
            last_received_dirty = 0;
        }
        SLEEP(gui_status_interval_ms);
    }
    write_gui_status();
    save_last_received_state();
    return NULL;
}

int main(int argc, char* argv[]) {
#ifdef WITH_PROFILING
    ProfilerStart("rtl_airband.prof");
#endif /* WITH_PROFILING */

#pragma GCC diagnostic ignored "-Wwrite-strings"
    char* cfgfile = CFGFILE;
    char* pidfile = PIDFILE;
#pragma GCC diagnostic warning "-Wwrite-strings"

    int opt;
    char optstring[16] = "efFhvc:P";

#ifdef NFM
    strcat(optstring, "Q");
#endif /* NFM */

#ifdef DEBUG
    strcat(optstring, "d:");
#endif /* DEBUG */

    int foreground = 0;  // daemonize
    int do_syslog = 1;

    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
#ifdef NFM
            case 'Q':
                fm_demod = FM_QUADRI_DEMOD;
                break;
#endif /* NFM */

#ifdef DEBUG
            case 'd':
                debug_path = strdup(optarg);
                break;
#endif /* DEBUG */

            case 'e':
                do_syslog = 0;
                break;
            case 'f':
                foreground = 1;
                tui = 1;
                break;
            case 'F':
                foreground = 1;
                tui = 0;
                break;
            case 'c':
                cfgfile = optarg;
                break;
            case 'P':
                print_cluster_scan_plan = true;
                do_syslog = 0;
                break;
            case 'v':
                cout << "RTLSDR-Airband version " << RTL_AIRBAND_VERSION << "\n";
                exit(EXIT_SUCCESS);
            case 'h':
            default:
                usage();
                break;
        }
    }
#ifdef DEBUG
    if (!debug_path)
        debug_path = strdup(DEBUG_PATH);
    init_debug(debug_path);
#endif /* DEBUG */

    // If executing other than as root, GPU memory gets alloc'd and the
    // 'permission denied' message on /dev/mem kills rtl_airband without
    // releasing GPU memory.
#ifdef WITH_BCM_VC
    // XXX should probably do this check in other circumstances also.
    if (0 != getuid()) {
        cerr << "FFT library requires that rtl_airband be executed as root\n";
        exit(1);
    }
#endif /* WITH_BCM_VC */

    // read config
    try {
        Config config;
        config.readFile(cfgfile);
        Setting& root = config.getRoot();
        if (root.exists("pidfile"))
            pidfile = strdup(root["pidfile"]);
        if (root.exists("fft_size")) {
            int fsize = (int)(root["fft_size"]);
            fft_size_log = 0;
            for (size_t i = MIN_FFT_SIZE_LOG; i <= MAX_FFT_SIZE_LOG; i++) {
                if (fsize == 1 << i) {
                    fft_size = (size_t)fsize;
                    fft_size_log = i;
                    break;
                }
            }
            if (fft_size_log == 0) {
                cerr << "Configuration error: invalid fft_size value (must be a power of two in range " << (1 << MIN_FFT_SIZE_LOG) << "-" << (1 << MAX_FFT_SIZE_LOG) << ")\n";
                error();
            }
        }
        if (root.exists("shout_metadata_delay"))
            shout_metadata_delay = (int)(root["shout_metadata_delay"]);
        if (shout_metadata_delay < 0 || shout_metadata_delay > 2 * TAG_QUEUE_LEN) {
            cerr << "Configuration error: shout_metadata_delay is out of allowed range (0-" << 2 * TAG_QUEUE_LEN << ")\n";
            error();
        }
        if (root.exists("localtime") && (bool)root["localtime"] == true)
            use_localtime = true;
        if (root.exists("multiple_demod_threads") && (bool)root["multiple_demod_threads"] == true) {
#ifdef WITH_BCM_VC
            cerr << "Using multiple_demod_threads not supported with BCM VideoCore for FFT\n";
            exit(1);
#endif /* WITH_BCM_VC */

            multiple_demod_threads = true;
        }
        if (root.exists("multiple_output_threads") && (bool)root["multiple_output_threads"] == true) {
            multiple_output_threads = true;
        }
        if (root.exists("log_scan_activity") && (bool)root["log_scan_activity"] == true)
            log_scan_activity = true;
        configure_scan_tui_colors(root);
        if (root.exists("stats_filepath"))
            stats_filepath = strdup(root["stats_filepath"]);
        if (root.exists("gui_status_filepath"))
            gui_status_filepath = strdup(root["gui_status_filepath"]);
        if (root.exists("gui_last_received_filepath"))
            gui_last_received_filepath = strdup(root["gui_last_received_filepath"]);
        if (root.exists("gui_status_interval_ms")) {
            gui_status_interval_ms = (int)root["gui_status_interval_ms"];
            if (gui_status_interval_ms < 100) {
                cerr << "Configuration error: gui_status_interval_ms must be at least 100\n";
                error();
            }
        }
#ifdef NFM
        if (root.exists("tau"))
            alpha = ((int)root["tau"] == 0 ? 0.0f : exp(-1.0f / (WAVE_RATE * 1e-6 * (int)root["tau"])));
#endif /* NFM */

        Setting& devs = config.lookup("devices");
        device_count = devs.getLength();
        if (device_count < 1) {
            cerr << "Configuration error: no devices defined\n";
            error();
        }

        struct sigaction sigact, pipeact;

        memset(&sigact, 0, sizeof(sigact));
        memset(&pipeact, 0, sizeof(pipeact));
        pipeact.sa_handler = SIG_IGN;
        sigact.sa_handler = &sighandler;
        sigaction(SIGPIPE, &pipeact, NULL);
        sigaction(SIGHUP, &sigact, NULL);
        sigaction(SIGINT, &sigact, NULL);
        sigaction(SIGQUIT, &sigact, NULL);
        sigaction(SIGTERM, &sigact, NULL);

        devices = (device_t*)XCALLOC(device_count, sizeof(device_t));
        shout_init();

        if (do_syslog) {
            openlog("rtl_airband", LOG_PID, LOG_DAEMON);
            log_destination = SYSLOG;
        } else if (foreground) {
            log_destination = STDERR;
        } else {
            log_destination = NONE;
        }

        if (root.exists("mixers")) {
            Setting& mx = config.lookup("mixers");
            mixers = (mixer_t*)XCALLOC(mx.getLength(), sizeof(struct mixer_t));
            if ((mixer_count = parse_mixers(mx)) > 0) {
                mixers = (mixer_t*)XREALLOC(mixers, mixer_count * sizeof(struct mixer_t));
            } else {
                free(mixers);
            }
        } else {
            mixer_count = 0;
        }

        uint32_t devs_enabled = parse_devices(devs);
        if (devs_enabled < 1) {
            cerr << "Configuration error: no devices defined\n";
            error();
        }
        device_count = devs_enabled;
        debug_print("mixer_count=%d\n", mixer_count);
#ifdef DEBUG
        for (int z = 0; z < mixer_count; z++) {
            mixer_t* m = &mixers[z];
            debug_print("mixer[%d]: name=%s, input_count=%d, output_count=%d\n", z, m->name, m->input_count, m->channel.output_count);
        }
#endif /* DEBUG */
    } catch (const FileIOException& e) {
        cerr << "Cannot read configuration file " << cfgfile << "\n";
        error();
    } catch (const ParseException& e) {
        cerr << "Error while parsing configuration file " << cfgfile << " line " << e.getLine() << ": " << e.getError() << "\n";
        error();
    } catch (const SettingNotFoundException& e) {
        cerr << "Configuration error: mandatory parameter missing: " << e.getPath() << "\n";
        error();
    } catch (const SettingTypeException& e) {
        cerr << "Configuration error: invalid parameter type: " << e.getPath() << "\n";
        error();
    } catch (const ConfigException& e) {
        cerr << "Unhandled config exception\n";
        error();
    }

    for (int i = 0; i < device_count; i++) {
        if (devices[i].cluster_scan_print_plan) {
            print_cluster_scan_plan = true;
        }
    }

    if (print_cluster_scan_plan) {
        print_cluster_plan_stdout();
        return 0;
    }

    load_last_received_state();
    log(LOG_INFO, "RTLSDR-Airband version %s starting\n", RTL_AIRBAND_VERSION);
    log_cluster_scan_plans();

    if (!foreground) {
        int pid1, pid2;
        if ((pid1 = fork()) == -1) {
            cerr << "Cannot fork child process: " << strerror(errno) << "\n";
            error();
        }
        if (pid1) {
            waitpid(-1, NULL, 0);
            return (0);
        } else {
            if ((pid2 = fork()) == -1) {
                cerr << "Cannot fork child process: " << strerror(errno) << "\n";
                error();
            }
            if (pid2) {
                return (0);
            } else {
                int nullfd, dupfd;
                if ((nullfd = open("/dev/null", O_RDWR)) == -1) {
                    log(LOG_CRIT, "Cannot open /dev/null: %s\n", strerror(errno));
                    error();
                }
                for (dupfd = 0; dupfd <= 2; dupfd++) {
                    if (dup2(nullfd, dupfd) == -1) {
                        log(LOG_CRIT, "dup2(): %s\n", strerror(errno));
                        error();
                    }
                }
                if (nullfd > 2)
                    close(nullfd);
                FILE* f = fopen(pidfile, "w");
                if (f == NULL) {
                    log(LOG_WARNING, "Cannot write pidfile: %s\n", strerror(errno));
                } else {
                    fprintf(f, "%ld\n", (long)getpid());
                    fclose(f);
                }
            }
        }
    }

    for (int i = 0; i < mixer_count; i++) {
        if (mixers[i].enabled == false) {
            continue;  // no inputs connected = no need to initialize output
        }
        channel_t* channel = &mixers[i].channel;
        for (int k = 0; k < channel->output_count; k++) {
            output_t* output = channel->outputs + k;
            if (!init_output(channel, output)) {
                cerr << "Failed to initialize mixer " << i << " output " << k << " - aborting\n";
                error();
            }
        }
    }
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;

            for (int k = 0; k < channel->output_count; k++) {
                output_t* output = channel->outputs + k;
                if (!init_output(channel, output)) {
                    cerr << "Failed to initialize device " << i << " channel " << j << " output " << k << " - aborting\n";
                    error();
                }
            }
        }
        if (input_init(dev->input) != 0 || dev->input->state != INPUT_INITIALIZED) {
            if (errno != 0) {
                cerr << "Failed to initialize input device " << i << ": " << strerror(errno) << " - aborting\n";
            } else {
                cerr << "Failed to initialize input device " << i << " - aborting\n";
            }
            error();
        }
        if (input_start(dev->input) != 0) {
            cerr << "Failed to start input on device " << i << ": " << strerror(errno) << " - aborting\n";
            error();
        }
        if (dev->mode == R_SCAN) {
            // FIXME: set errno
            if (pthread_mutex_init(&dev->tag_queue_lock, NULL) != 0) {
                cerr << "Failed to initialize mutex - aborting\n";
                error();
            }
            // FIXME: not needed when freq_count == 1?
            pthread_create(&dev->controller_thread, NULL, &controller_thread, dev);
        } else if (dev->mode == R_CLUSTER_SCAN) {
            activate_cluster(dev, 0);
            pthread_create(&dev->controller_thread, NULL, &cluster_controller_thread, dev);
        }
    }

    int timeout = 50;  // 5 seconds
    while ((devices_running = count_devices_running()) != device_count && timeout > 0) {
        SLEEP(100);
        timeout--;
    }
    if ((devices_running = count_devices_running()) != device_count) {
        log(LOG_ERR, "%d device(s) failed to initialize - aborting\n", device_count - devices_running);
        error();
    }
    if (tui) {
        printf("\e[1;1H\e[2J");

        GOTOXY(0, 0);
        printf("                                                                               ");
        for (int i = 0; i < device_count; i++) {
            GOTOXY(0, i * 17 + 1);
            if (devices[i].mode == R_SCAN) {
                printf("%s", scan_tui_palette.title.c_str());
                scan_tui_fill_line(' ');
                GOTOXY(1, i * 17 + 1);
                printf("RTLSDR-Airband scan  dev %d  freqs %d", i, devices[i].channels[0].freq_count);
                printf("\033[0m");
                GOTOXY(0, i * 17 + 2);
                printf("%s%8s  %-4s  %5s  %4s  %5s  %7s  %s\033[0m", scan_tui_palette.header.c_str(), "MHz", "STATE", "IDX", "SIG", "NOISE", "HITS", "LABEL");
            } else if (devices[i].mode == R_CLUSTER_SCAN) {
                cluster_tui_print_summary(devices + i, i);
                for (int row = 4; row < 16; row++) {
                    GOTOXY(0, i * 17 + row);
                    printf("\033[K");
                }
            } else {
                for (int j = 0; j < devices[i].channel_count; j++) {
                    printf(" %7.3f  ", devices[i].channels[j].freqlist[devices[i].channels[j].freq_idx].frequency / 1000000.0);
                }
            }
            if (i != device_count - 1) {
                GOTOXY(0, i * 17 + 16);
                printf("-------------------------------------------------------------------------------");
            }
        }
    }
    THREAD output_check;
    pthread_create(&output_check, NULL, &output_check_thread, NULL);

    int demod_thread_count = multiple_demod_threads ? device_count : 1;
    demod_params_t* demod_params = (demod_params_t*)XCALLOC(demod_thread_count, sizeof(demod_params_t));
    THREAD* demod_threads = (THREAD*)XCALLOC(demod_thread_count, sizeof(THREAD));

    int output_thread_count = 1;
    if (multiple_output_threads) {
        output_thread_count = demod_thread_count;
        if (mixer_count > 0) {
            output_thread_count++;
        }
    }
    output_params_t* output_params = (output_params_t*)XCALLOC(output_thread_count, sizeof(output_params_t));
    THREAD* output_threads = (THREAD*)XCALLOC(output_thread_count, sizeof(THREAD));

    // Setup the output and demod threads
    if (multiple_output_threads == false) {
        init_output_params(&output_params[0], 0, device_count, 0, mixer_count);

        if (multiple_demod_threads == false) {
            init_demod(&demod_params[0], output_params[0].mp3_signal, 0, device_count);
        } else {
            for (int i = 0; i < demod_thread_count; i++) {
                init_demod(&demod_params[i], output_params[0].mp3_signal, i, i + 1);
            }
        }
    } else {
        if (multiple_demod_threads == false) {
            init_output_params(&output_params[0], 0, device_count, 0, 0);
            init_demod(&demod_params[0], output_params[0].mp3_signal, 0, device_count);
        } else {
            for (int i = 0; i < device_count; i++) {
                init_output_params(&output_params[i], i, i + 1, 0, 0);
                init_demod(&demod_params[i], output_params[i].mp3_signal, i, i + 1);
            }
        }
        if (mixer_count > 0) {
            init_output_params(&output_params[output_thread_count - 1], 0, 0, 0, mixer_count);
        }
    }

    // Startup the output threads
    for (int i = 0; i < output_thread_count; i++) {
        pthread_create(&output_threads[i], NULL, &output_thread, &output_params[i]);
    }

    // Startup the mixer thread (if there is one) using the signal for the last output thread
    THREAD mixer;
    if (mixer_count > 0) {
        pthread_create(&mixer, NULL, &mixer_thread, output_params[output_thread_count - 1].mp3_signal);
    }

#ifdef WITH_PULSEAUDIO
    pulse_start();
#endif /* WITH_PULSEAUDIO */

    sincosf_lut_init();

    // Startup the demod threads
    for (int i = 0; i < demod_thread_count; i++) {
        pthread_create(&demod_threads[i], NULL, &demodulate, &demod_params[i]);
    }

    THREAD gui_thread;
    bool gui_thread_started = false;
    if (gui_status_filepath != NULL) {
        pthread_create(&gui_thread, NULL, &gui_status_thread, NULL);
        gui_thread_started = true;
    }

    // Wait for demod threads to exit
    for (int i = 0; i < demod_thread_count; i++) {
        pthread_join(demod_threads[i], NULL);
    }
    if (gui_thread_started) {
        pthread_join(gui_thread, NULL);
    }

    log(LOG_INFO, "Cleaning up\n");
    for (int i = 0; i < device_count; i++) {
        if (devices[i].mode == R_SCAN || devices[i].mode == R_CLUSTER_SCAN)
            pthread_join(devices[i].controller_thread, NULL);
        if (input_stop(devices[i].input) != 0 || devices[i].input->state != INPUT_STOPPED) {
            if (errno != 0) {
                log(LOG_ERR, "Failed do stop device #%d: %s\n", i, strerror(errno));
            } else {
                log(LOG_ERR, "Failed do stop device #%d\n", i);
            }
        }
    }
    log(LOG_INFO, "Input threads closed\n");

    if (mixer_count > 0) {
        log(LOG_INFO, "Closing mixer thread\n");
        pthread_join(mixer, NULL);
    }

    log(LOG_INFO, "Closing output thread(s)\n");
    for (int i = 0; i < output_thread_count; i++) {
        output_params[i].mp3_signal->send();
        pthread_join(output_threads[i], NULL);
    }

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        disable_device_outputs(dev);
    }

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                output_t* output = channel->outputs + k;
                if (output->lame) {
                    lame_close(output->lame);
                }
            }
        }
    }

    close_debug();
#ifdef WITH_PROFILING
    ProfilerStop();
#endif /* WITH_PROFILING */
    return 0;
}
