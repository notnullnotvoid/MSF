//TODO: write timing code that doesn't rely on instrumentation inside giff.c

// #include "giff.h"
#include "msf_gif.h"
#include "List.hpp"
#include "common.hpp"

#include <stdio.h>
#include <SDL2/SDL.h>

u64 applicationStartupTimeValue;

extern "C" double get_time() {
    u64 currentTimeValue = SDL_GetPerformanceCounter();
    u64 diffTimeValue = currentTimeValue - applicationStartupTimeValue;
    double elapsedSeconds = (double)diffTimeValue / (double)SDL_GetPerformanceFrequency();
    return elapsedSeconds;
}

struct RawBlob {
    int width, height, frames, centiSeconds;
    int pixels[1];
};

// #include <unistd.h>

int main() {
    applicationStartupTimeValue = SDL_GetPerformanceCounter();
    // printf("logical cores available: %ld\n", sysconf(_SC_NPROC/ESSORS_ONLN));
    // const char * names[] = { "bouncy", "diwide-large", "diwide", "floor", "increase", "keyhole", "odd", "sky", "tiles" };
    const char * names[] = { "bouncy", "diwide", "increase" };

    List<RawBlob *> blobs = {};
    for (const char * name : names) {
        char * path = dsprintf(nullptr, "in/%s.rawframes", name);
        printf("loading %s...\n", path); fflush(stdout);
        blobs.add((RawBlob *) read_entire_file(path));
    }

    struct TimerInfo {
        double time;
        size_t in, out;
    };
    List<TimerInfo> timers = {};

    //TODO: write both dithered and non-dithered versions each to their own folder?
    //TODO: automatically regression-test against known good versions of the GIFs?
    for (int i = 0; i < blobs.len; ++i) {
        //converting BGRA to RGBA and initializing list of frames
        RawBlob * blob = blobs[i];
        List<uint8_t *> frames = {};
        for (int j = 0; j < blob->frames; ++j) {
            frames.add((u8 *) &blob->pixels[blob->width * blob->height * j]);
            for (int k = 0; k < blob->width * blob->height; ++k) {
                u8 * p = (u8 *) &blob->pixels[blob->width * blob->height * j + k];
                swap(p[0], p[2]);
            }
        }

        //write gifs using the new algorithm
        char * path = dsprintf(nullptr, "new/%s.gif", names[i]);
        printf("writing %20s      width: %d   height: %d   frames: %d   centiSeconds: %d\n",
            path, blob->width, blob->height, blob->frames, blob->centiSeconds); fflush(stdout);
        double pre = get_time();
        size_t out = msf_save_gif(frames.data, frames.len, blob->width, blob->height, blob->centiSeconds, path);
        timers.add({ get_time() - pre, (size_t)(blob->frames * blob->width * blob->height * 4), out });

    }

    printf("\n");
    TimerInfo totals = {};
    for (int i = 0; i < timers.len; ++i) {
        totals.time += timers[i].time;
        totals.in += timers[i].in;
        totals.out += timers[i].out;
        printf("%16s      time: %6.3f   in: %4.2f GB/s   out: %5.2f MB/s\n",
            names[i], timers[i].time,
            timers[i].in / timers[i].time / 1024 / 1024 / 1024, timers[i].out / timers[i].time / 1024 / 1024);
    }
    printf("\n%16s      time: %6.3f   in: %4.2f GB/s   out: %5.2f MB/s\n",
        "totals", totals.time, totals.in / totals.time / 1024 / 1024 / 1024, totals.out / totals.time / 1024 / 1024);

    return 0;
}