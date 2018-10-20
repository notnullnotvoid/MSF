//NOTE: AVX2 doesn't appear to make the exporter any faster overall, so it's removed for now
//TODO: figure out why they aren't faster

//TODO: combine rbits/gbits/bbits into a struct to reduce verbosity?

#include "giff.hpp"
#include "common.hpp"
#include "FileBuffer.hpp"

#include <pthread.h>
#include <emmintrin.h>

struct ThreadBarrier {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int flag, count, num;
};

static void barrier_init(ThreadBarrier * bar, int num) {
    *bar = {};
    pthread_mutex_init(&bar->mutex, nullptr);
    pthread_cond_init(&bar->cond, nullptr);
    bar->num = num;
}

static void barrier_wait(ThreadBarrier * bar) {
    pthread_mutex_lock(&bar->mutex);
    int flag = bar->flag;
    // int count = __sync_fetch_and_add(bar->count, 1);
    // if (count + 1 == bar->num) {
    ++bar->count;
    if (bar->count == bar->num) {
        bar->count = 0;
        bar->flag = !bar->flag;
        pthread_cond_broadcast(&bar->cond);
    } else {
        while (bar->flag == flag) {
            pthread_cond_wait(&bar->cond, &bar->mutex);
        }
    }
    pthread_mutex_unlock(&bar->mutex);
}

static void barrier_destroy(ThreadBarrier * bar) {
    pthread_mutex_destroy(&bar->mutex);
    pthread_cond_destroy(&bar->cond);
}

#ifdef _MSC_VER
    static int bit_log(int i) {
        unsigned long idx;
        _BitScanReverse(&idx, i) + 1;
        return idx;
    }
#else
    //TODO: only use this version on clang and gcc,
    static int bit_log(int i) {
        return 32 - __builtin_clz(i);
    }
    //TODO: add a generic version of bit_log() using de bruijn multiplication
#endif

//forward declaration
double get_time();

struct BlockBuffer {
    u32 bits;
    u16 bytes[129];
};

static void put_code(FileBuffer * buf, BlockBuffer * block, int bits, u32 code) {
    //insert new code into block buffer
    int idx = block->bits / 16;
    int bit = block->bits % 16;
    block->bytes[idx + 0] |= code <<        bit      ;
    block->bytes[idx + 1] |= code >>  (16 - bit)     ;
    block->bits += bits;

    //flush the block buffer if it's full
    if (block->bits >= 255 * 8) {
        buf->check(256);
        buf->write_unsafe<u8>(255);
        buf->write_block_unsafe<u8>((u8 *) block->bytes, 255);

        block->bits -= 255 * 8;
        block->bytes[0] = block->bytes[127] >> 8 | block->bytes[128] << 8;
        memset(block->bytes + 1, 0, 256);
    }
}

struct StridedList {
    i16 * data;
    size_t len;
    size_t stride;

    i16 * operator[](size_t index) {
        return &data[index * stride];
    }
};

static void reset(StridedList * lzw, int tableSize, int stride) {
    memset(lzw->data, 0xFF, 4096 * stride * sizeof(i16));
    lzw->len = tableSize + 2;
    lzw->stride = stride;
}

struct MetaPaletteInfo {
    bool * * used;
    int rbits, gbits, bbits;
};

static bool simd_friendly(PixelFormat f) {
    return f.stride == 4 && f.ridx >= 0 && f.ridx < 4
                         && f.gidx >= 0 && f.gidx < 4
                         && f.bidx >= 0 && f.bidx < 4;
}

static void cook_frame_part(RawFrame raw, u32 * cooked, int width, int miny, int maxy,
                     int rbits, int gbits, int bbits, PixelFormat format, bool * used)
{
    if (simd_friendly(format)) {
        int rshift = format.ridx * 8 + 8 - rbits;
        int gshift = format.gidx * 8 + 8 - gbits;
        int bshift = format.bidx * 8 + 8 - bbits;
        __m128i rmask = _mm_set1_epi32((1 << rbits) - 1);
        __m128i gmask = _mm_set1_epi32((1 << gbits) - 1);
        __m128i bmask = _mm_set1_epi32((1 << bbits) - 1);

        // int rshift = format.ridx * 8 + 8 - rbits;
        // int gshift = format.gidx * 8 + 8 - rbits - gbits;
        // int bshift = format.bidx * 8 + 8 - rbits - gbits - bbits;
        // // __m128i rmask = _mm_set1_epi32(((1 << rbits) - 1) << (format.ridx * 8 + 8 - rbits));
        // // __m128i gmask = _mm_set1_epi32(((1 << gbits) - 1) << (format.gidx * 8 + 8 - gbits));
        // // __m128i bmask = _mm_set1_epi32(((1 << bbits) - 1) << (format.bidx * 8 + 8 - bbits));
        // __m128i rmask = _mm_set1_epi32(((1 << rbits) - 1));
        // __m128i gmask = _mm_set1_epi32(((1 << gbits) - 1) << rbits);
        // __m128i bmask = _mm_set1_epi32(((1 << bbits) - 1) << rbits << gbits);

        for (int y = miny; y < maxy; ++y) {
            int x = 0;
            //NOTE: If we could guarantee the channels are in the right byte order (R,G,B)
            //      then we could get away with masking first and only doing 3 right-shifts.
            //      This is relevant because we might be getting hamstrung by the fact that we can
            //      only issue one vector shift instruction per cycle.
            for (; x < width - 3; x += 4) {
                u8 * p = &raw.pixels[y * raw.pitch + x * format.stride];
                u32 * c = &cooked[y * width + x];
                __m128i in = _mm_loadu_si128((__m128i *) p);

                __m128i r = _mm_and_si128(_mm_srli_epi32(in, rshift), rmask);
                __m128i g = _mm_and_si128(_mm_srli_epi32(in, gshift), gmask);
                __m128i b = _mm_and_si128(_mm_srli_epi32(in, bshift), bmask);
                g = _mm_slli_epi32(g, rbits);
                b = _mm_slli_epi32(b, rbits + gbits);
                __m128i out = _mm_or_si128(r, _mm_or_si128(g, b));

                // // __m128i r = _mm_srli_epi32(_mm_and_si128(in, rmask), rshift);
                // // __m128i g = _mm_srli_epi32(_mm_and_si128(in, gmask), gshift);
                // // __m128i b = _mm_srli_epi32(_mm_and_si128(in, bmask), bshift);
                // __m128i r = _mm_and_si128(_mm_srli_epi32(in, rshift), rmask);
                // __m128i g = _mm_and_si128(_mm_srli_epi32(in, gshift), gmask);
                // __m128i b = _mm_and_si128(_mm_srli_epi32(in, bshift), bmask);
                // __m128i out = _mm_or_si128(r, _mm_or_si128(g, b));

                _mm_storeu_si128((__m128i *) c, out);

                //mark colors
                used[c[0]] = true;
                used[c[1]] = true;
                used[c[2]] = true;
                used[c[3]] = true;
            }

            for (; x < width; ++x) {
                u8 * p = &raw.pixels[y * raw.pitch + x * format.stride];
                cooked[y * width + x] = p[format.bidx] >> (8 - bbits) << (rbits + gbits) |
                                        p[format.gidx] >> (8 - gbits) <<  rbits          |
                                        p[format.ridx] >> (8 - rbits);
                used[cooked[y * width + x]] = true; //mark colors
            }
        }
    } else {
        for (int y = miny; y < maxy; ++y) {
            for (int x = 0; x < width; ++x) {
                u8 * p = &raw.pixels[y * raw.pitch + x * format.stride];
                cooked[y * width + x] = p[format.bidx] >> (8 - bbits) << (rbits + gbits) |
                                        p[format.gidx] >> (8 - gbits) <<  rbits          |
                                        p[format.ridx] >> (8 - rbits);
                used[cooked[y * width + x]] = true; //mark colors
            }
        }
    }
}

static void cook_frame_part_dithered(RawFrame raw, u32 * cooked, int width, int miny, int maxy,
                         int rbits, int gbits, int bbits, PixelFormat fmt, bool * used)
{
    int ditherKernel[8 * 8] = {
         0, 48, 12, 60,  3, 51, 15, 63,
        32, 16, 44, 28, 35, 19, 47, 31,
         8, 56,  4, 52, 11, 59,  7, 55,
        40, 24, 36, 20, 43, 27, 39, 23,
         2, 50, 14, 62,  1, 49, 13, 61,
        34, 18, 46, 30, 33, 17, 45, 29,
        10, 58,  6, 54,  9, 57,  5, 53,
        42, 26, 38, 22, 41, 25, 37, 21,
    };

    if (simd_friendly(fmt) && !(fmt.ridx == fmt.gidx || fmt.ridx == fmt.bidx)) {
        //TODO: is the cost of deriving the dither kernel significant to performance?
        int derivedKernel[8 * 8];
        for (int i = 0; i < 8 * 8; ++i) {
            int k = ditherKernel[i];
            derivedKernel[i] = k >> (rbits - 2) << fmt.ridx * 8 |
                               k >> (gbits - 2) << fmt.gidx * 8 |
                               k >> (bbits - 2) << fmt.bidx * 8;
        }

        int rshift = fmt.ridx * 8 + 8 - rbits;
        int gshift = fmt.gidx * 8 + 8 - gbits;
        int bshift = fmt.bidx * 8 + 8 - bbits;
        __m128i rmask = _mm_set1_epi32((1 << rbits) - 1);
        __m128i gmask = _mm_set1_epi32((1 << gbits) - 1);
        __m128i bmask = _mm_set1_epi32((1 << bbits) - 1);
        for (int y = miny; y < maxy; ++y) {
            int x = 0;
            for (; x < width - 3; x += 4) {
                u8 * p = &raw.pixels[y * raw.pitch + x * fmt.stride];
                u32 * c = &cooked[y * width + x];
                int dx = x & 7, dy = y & 7;
                int * kp = &derivedKernel[dy * 8 + dx];

                __m128i in = _mm_loadu_si128((__m128i *) p);
                __m128i k = _mm_loadu_si128((__m128i *) kp);
                in = _mm_adds_epu8(in, k);
                __m128i r = _mm_and_si128(_mm_srli_epi32(in, rshift), rmask);
                __m128i g = _mm_and_si128(_mm_srli_epi32(in, gshift), gmask);
                __m128i b = _mm_and_si128(_mm_srli_epi32(in, bshift), bmask);
                g = _mm_slli_epi32(g, rbits);
                b = _mm_slli_epi32(b, rbits + gbits);
                __m128i out = _mm_or_si128(r, _mm_or_si128(g, b));
                _mm_storeu_si128((__m128i *) c, out);

                //mark colors
                used[c[0]] = true;
                used[c[1]] = true;
                used[c[2]] = true;
                used[c[3]] = true;
            }

            for (; x < width; ++x) {
                u8 * p = &raw.pixels[y * raw.pitch + x * fmt.stride];
                int dx = x & 7, dy = y & 7;
                int k = ditherKernel[dy * 8 + dx];
                cooked[y * width + x] =
                    min(255, p[fmt.bidx] + (k >> (bbits - 2))) >> (8 - bbits) << (rbits + gbits) |
                    min(255, p[fmt.gidx] + (k >> (gbits - 2))) >> (8 - gbits) <<  rbits          |
                    min(255, p[fmt.ridx] + (k >> (rbits - 2))) >> (8 - rbits);
                used[cooked[y * width + x]] = true; //mark colors
            }
        }
    } else {
        for (int y = miny; y < maxy; ++y) {
            for (int x = 0; x < width; ++x) {
                u8 * p = &raw.pixels[y * raw.pitch + x * fmt.stride];
                int dx = x & 7, dy = y & 7;
                int k = ditherKernel[dy * 8 + dx];
                cooked[y * width + x] =
                    min(255, p[fmt.bidx] + (k >> (bbits - 2))) >> (8 - bbits) << (rbits + gbits) |
                    min(255, p[fmt.gidx] + (k >> (gbits - 2))) >> (8 - gbits) <<  rbits          |
                    min(255, p[fmt.ridx] + (k >> (rbits - 2))) >> (8 - rbits);
                used[cooked[y * width + x]] = true; //mark colors
            }
        }
    }
}

struct CookData {
    //constant
    int width, height;
    PixelFormat format;
    int totalThreads; //duplicated with info in barrier?
    bool * * used;
    bool dither;

    //non-constant
    bool done;
    ThreadBarrier barrier;

    //filled out from scratch every frame
    RawFrame raw;
    u32 * cooked;
    int rbits, gbits, bbits;
    int currentFrame; //DEBUG
};

struct CookLocal {
    pthread_t thread;
    int id;
    CookData * global;
};

static void cook_frame(CookData * data, int id) {
    //TODO: prove this won't ever exclude the last line from computation
    int miny =  id      * (data->height / (float) data->totalThreads);
    int maxy = (id + 1) * (data->height / (float) data->totalThreads);

    bool * used = data->used[id];
    int paletteSize = 1 << (data->rbits + data->gbits + data->bbits);
    memset(used, 0, paletteSize * sizeof(bool));

    if (data->dither)
        cook_frame_part_dithered(data->raw, data->cooked, data->width, miny, maxy,
                                 data->rbits, data->gbits, data->bbits, data->format, used);
    else
        cook_frame_part(data->raw, data->cooked, data->width, miny, maxy,
                        data->rbits, data->gbits, data->bbits, data->format, used);
}

static void * cook_frames(void * arg) {
    CookLocal * local = (CookLocal *) arg;
    CookData * data = local->global;
    while (true) {
        barrier_wait(&data->barrier);
        if (data->done) { return nullptr; }
        cook_frame(data, local->id);
        barrier_wait(&data->barrier);
    }
    return nullptr;
}

static MetaPaletteInfo choose_meta_palette(List<RawFrame> rawFrames, List<u32 *> cookedFrames,
        int width, int height, bool dither, PixelFormat format,
        int threadCount, DebugTimers & timers)
{
    int pixelsPerBatch = 4096 * 8; //arbitrary number which can be tuned for performance
    int linesPerBatch = (pixelsPerBatch + width - 1) / width; //round up
    int maxCookThreads = max(1, height / linesPerBatch);
    int poolSize = min(threadCount - 1, maxCookThreads - 1);
    printf("pixelsPerBatch %d   linesPerBatch %d   maxCookThreads %d   poolSize %d\n",
        pixelsPerBatch, linesPerBatch, maxCookThreads, poolSize);

    bool * threadUsedMem = (bool *) malloc((poolSize + 1) * (1 << 15));
    bool * threadUsed[poolSize + 1];
    for (int i = 0; i < poolSize + 1; ++i)
        threadUsed[i] = &threadUsedMem[i * (1 << 15)];
    CookData cookData = { width, height, format, poolSize + 1, threadUsed, dither };
    barrier_init(&cookData.barrier, poolSize + 1);

    //spawn thread pool for cooking
    CookLocal threads[poolSize];
    for (int i = 0; i < poolSize; ++i) {
        threads[i].id = i;
        threads[i].global = &cookData;
        pthread_create(&threads[i].thread, nullptr, cook_frames, &threads[i]);
    }

    bool * * used = (bool * *) malloc(rawFrames.len * sizeof(bool *));
    //set to null so we can spuriously free() with no effect
    memset(used, 0, rawFrames.len * sizeof(bool *));

    //bit depth for each channel
    //TODO: generate these programmatically so we can get rid of the arrays
    int rbits[9] = { 5, 5, 4, 4, 4, 3, 3, 3, 2 };
    int gbits[9] = { 5, 5, 5, 4, 4, 4, 3, 3, 3 };
    int bbits[9] = { 5, 4, 4, 4, 3, 3, 3, 2, 2 };

    //NOTE: it's possible (if unlikely) for a reduction in bit depth to result in an increase in
    //      colors used for a given frame when dithering is applied, so we treat the list of frames
    //      like a ring buffer, circling around until all frames are cooked with the same bit depth
    int minPalette = 0;
    int minCorrect = 0;
    int idx = 0;
    while (idx < minCorrect + (int) cookedFrames.len - 1) {
        int i = idx % (cookedFrames.len - 1) + 1;
        u32 * frame = cookedFrames[i];
        int paletteSize = 1 << (15 - minPalette);
        //TODO: use fewer allocations here?
        free(used[i - 1]);
        used[i - 1] = (bool *) malloc(paletteSize * sizeof(bool));

        float preCook = get_time();
        cookData.raw = rawFrames[i - 1];
        cookData.cooked = frame;
        cookData.rbits = rbits[minPalette];
        cookData.gbits = gbits[minPalette];
        cookData.bbits = bbits[minPalette];
        cookData.currentFrame = idx;

        barrier_wait(&cookData.barrier);
        cook_frame(&cookData, poolSize);
        barrier_wait(&cookData.barrier);

        timers.cook += get_time() - preCook;

        //mark down which colors are used out of the full 15-bit palette
        float preCount = get_time();
        //count how many fall into the meta-palette
        memset(used[i - 1], 0, paletteSize * sizeof(bool));
        for (int k = 0; k < poolSize + 1; ++k)
            for (int j = 0; j < paletteSize; ++j)
                used[i - 1][j] |= threadUsed[k][j];
        int count = 0;
        for (int j = 0; j < paletteSize; ++j)
            if (used[i - 1][j])
                ++count;
        timers.count += get_time() - preCount;

        if (count < 256) {
            ++idx;
        } else {
            ++minPalette;
            minCorrect = idx;
        }
    }

    cookData.done = true;
    barrier_wait(&cookData.barrier);

    //NOTE: we need to wait for all threads to finish so that we know they've had time
    //      to read data->done (and exit the loop) before we clobber this stack frame
    for (int i = 0; i < poolSize; ++i) {
        pthread_join(threads[i].thread, nullptr);
    }
    barrier_destroy(&cookData.barrier);
    free(threadUsedMem);

    return { used, rbits[minPalette], gbits[minPalette], bbits[minPalette] };
}

struct CompressionData {
    int width, height, centiSeconds;
    int frameIdx;
    MetaPaletteInfo meta;
    List<u32 *> cookedFrames;
    List<FileBuffer> compressedFrames;
};

static void * compress_frames(void * arg) {
    CompressionData * data = (CompressionData *) arg;
    MetaPaletteInfo meta = data->meta;
    int width = data->width, height = data->height, centiSeconds = data->centiSeconds;
    // StridedList lzw = { (i16 *) malloc(4096 * (meta.maxUsed + 1) * sizeof(i16)) };
    StridedList lzw = { (i16 *) malloc(4096 * 256 * sizeof(i16)) };
    u8 idxBuffer[4096];
    int idxLen = 0;

    while (true) {
        int frameIdx = __sync_fetch_and_add(&data->frameIdx, 1);
        if (frameIdx >= (int) data->cookedFrames.len) {
            free(lzw.data);
            return nullptr;
        }

        u32 * pframe = data->cookedFrames[frameIdx - 1];
        u32 * cframe = data->cookedFrames[frameIdx];
        FileBuffer buf = create_file_buffer(1024);

        //allocate tlb
        int totalBits = meta.rbits + meta.gbits + meta.bbits;
        int tlbSize = 1 << totalBits;
        u8 * tlb = (u8 *) malloc(tlbSize * sizeof(u8));

        //generate palette
        struct Color3 { u8 r, g, b; };
        Color3 table[256] = {};
        int tableIdx = 1; //we start counting at 1 because 0 is the transparent color
        for (int i = 0; i < tlbSize; ++i) {
            if (meta.used[frameIdx - 1][i]) {
                tlb[i] = tableIdx;
                int rmask = (1 << meta.rbits) - 1;
                int gmask = (1 << meta.gbits) - 1;
                //isolate components
                int r = i & rmask;
                int g = i >> meta.rbits & gmask;
                int b = i >> (meta.rbits + meta.gbits);
                //shift into highest bits
                r <<= 8 - meta.rbits;
                g <<= 8 - meta.gbits;
                b <<= 8 - meta.bbits;
                table[tableIdx] = {
                    (u8)(r | r >> meta.rbits),
                    (u8)(g | g >> meta.gbits),
                    (u8)(b | b >> meta.bbits),
                };
                ++tableIdx;
            }
        }

        free(meta.used[frameIdx - 1]);
        // printf("frame %d uses %d colors\n", j, tableIdx);

        int tableBits = bit_log(tableIdx - 1);
        int tableSize = 1 << tableBits;
        // printf("idx: %d bits: %d size: %d\n\n", tableIdx, tableBits, tableSize);

        buf.check(8 + 10);
        //graphics control extension
        buf.write_unsafe<u8>(0x21); //extension introducer
        buf.write_unsafe<u8>(0xF9); //extension identifier
        buf.write_unsafe<u8>(4); //block size (always 4)
        //reserved, disposal method:keep, input flag, transparency flag
        //NOTE: MSVC incorrectly generates warning C4806 here due to a compiler bug.
        // buf.write_unsafe<u8>(0b000'001'0'0 | (j != 1));
        buf.write_unsafe<u8>(0b00000100 | (frameIdx != 1));
        buf.write_unsafe<u16>(centiSeconds); //x/100 seconds per frame
        buf.write_unsafe<u8>(0); //transparent color index
        buf.write_unsafe<u8>(0); //block terminator

        //image descriptor
        buf.write_unsafe<u8>(0x2C); //image separator
        buf.write_unsafe<u16>(0); //image left
        buf.write_unsafe<u16>(0); //image top
        buf.write_unsafe<u16>(width);
        buf.write_unsafe<u16>(height);
        //local color table flag, interlace flag, sort flag, reserved, local color table size
        // buf.write_unsafe<u8>(0b1'0'0'00'000 | (tableBits - 1));
        buf.write_unsafe<u8>(0b10000000 | (tableBits - 1));

        //local color table
        buf.write_block(table, tableSize);

        //image data
        BlockBuffer block = {};
        buf.write<u8>(tableBits); //lzw minimum code size
        reset(&lzw, tableSize, tableIdx);
        //XXX: do we actually need to write this?
        put_code(&buf, &block, bit_log(lzw.len - 1), tableSize); //clear code

        int lastCode = cframe[0] == pframe[0]? 0 : tlb[cframe[0]];
        for (int i : range(1, width * height)) {
            idxBuffer[idxLen++] = cframe[i] == pframe[i]? 0 : tlb[cframe[i]];
            int code = lzw[lastCode][idxBuffer[idxLen - 1]];
            if (code < 0) {
                //write to code stream
                int codeBits = bit_log(lzw.len - 1);
                put_code(&buf, &block, codeBits, lastCode);
                // printf("%d-%d-%d  ", lastCode, codeBits, (int) lzw.len);

                //NOTE: [I THINK] we need to leave room for 2 more codes (leftover and end code)
                //      because we don't ever reset the table after writing the leftover bits
                //XXX: is my thinking correct on this one?
                if (lzw.len > 4094) {
                    //reset buffer code table
                    put_code(&buf, &block, codeBits, tableSize);
                    reset(&lzw, tableSize, tableIdx);
                } else {
                    lzw[lastCode][idxBuffer[idxLen - 1]] = lzw.len;
                    ++lzw.len;
                }

                //reset index buffer
                idxBuffer[0] = idxBuffer[idxLen - 1];
                idxLen = 1;

                lastCode = idxBuffer[0];
            } else {
                lastCode = code;
            }
        }

        free(tlb);

        //write code for leftover index buffer contents, then the end code
        put_code(&buf, &block, bit_log(lzw.len - 1), lastCode);
        put_code(&buf, &block, bit_log(lzw.len), tableSize + 1); //end code

        //flush remaining data
        if (block.bits) {
            int bytes = (block.bits + 7) / 8; //round up
            buf.check(bytes + 1);
            buf.write_unsafe<u8>(bytes);
            buf.write_block_unsafe<u8>((u8 *) block.bytes, bytes);
        }

        buf.write<u8>(0); //terminating block
        idxLen = 0; //reset encoding state

        data->compressedFrames[frameIdx - 1] = buf;
    }
}

//TODO:
//add very short GIF test case
//add very small GIF test case
//SIMD-ize color counting
////multithread color counting
////write thread barrier spinlock routines
////test performance using mutex vs. spinlock for thread barrier?
//use VLAs or alloca() wherever reasonable
//use fewer memory allocations
//allow specifying different thread counts for cooking and compression?
//dynamically determine physical/logical core count
//make threading code work on win/MSVC as well
//put threading code behind preprocessor option

DebugTimers save_gif(int width, int height, List<RawFrame> rawFrames, int centiSeconds,
                     const char * path, bool dither, PixelFormat format, int threadCount)
{
    DebugTimers timers = {};

    float preAmble, preTotal;
    preAmble = preTotal = get_time();

    //allocate space for cooked frames
    List<u32 *> cookedFrames = create_list<u32 *>(rawFrames.len + 1);
    u32 * cookedMemBlock = (u32 *) malloc((rawFrames.len + 1) * width * height * sizeof(u32));
    memset(cookedMemBlock, 0, width * height * sizeof(u32)); //set dummy frame to background color
    for (int i = 0; i < (int) rawFrames.len + 1; ++i) {
        cookedFrames.add(cookedMemBlock + width * height * i);
    }

    float preChoice = get_time();
    MetaPaletteInfo meta = choose_meta_palette(
        rawFrames, cookedFrames, width, height, dither, format, threadCount, timers);
    timers.choice = get_time() - preChoice;
    printf("bits: %d, %d, %d\n", meta.rbits, meta.gbits, meta.bbits);
    timers.amble = get_time() - preAmble;



    float preCompress = get_time();
    CompressionData compressData = { width, height, centiSeconds, 1, meta, cookedFrames,
                                   create_list<FileBuffer>(cookedFrames.len - 1) };
    compressData.compressedFrames.len = cookedFrames.len - 1;

    //spawn worker threads for compression
    int poolSize = min(rawFrames.len - 1, threadCount - 1);
    pthread_t threads[poolSize];
    //ensure that threads will be joinable (this is not guaranteed to be a default setting)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    for (int i = 0; i < poolSize; ++i) {
        pthread_create(&threads[i], &attr, compress_frames, &compressData);
    }
    pthread_attr_destroy(&attr);

    //header
    FileBuffer buf = create_file_buffer(2048);
    for (char c : range("GIF89a")) {
        buf.write_unsafe(c);
    }

    //logical screen descriptor
    buf.write_unsafe<u16>(width);
    buf.write_unsafe<u16>(height);
    //global color table flag, color resolution (???), sort flag, global color table size
    // buf.write_unsafe<u8>(0b0'001'0'000);
    buf.write_unsafe<u8>(0b00010000);
    buf.write_unsafe<u8>(0); //background color index
    buf.write_unsafe<u8>(0); //pixel aspect ratio

    //application extension
    buf.write_unsafe<u8>(0x21); //extension introducer
    buf.write_unsafe<u8>(0xFF); //extension identifier
    buf.write_unsafe<u8>(11); //fixed length data size
    for (char c : range("NETSCAPE2.0")) {
        buf.write_unsafe(c);
    }
    buf.write_unsafe<u8>(3); //data block size
    buf.write_unsafe<u8>(1); //???
    buf.write_unsafe<u16>(0); //loop forever
    buf.write_unsafe<u8>(0); //block terminator

    compress_frames(&compressData);

    //wait for threads to finish and round up results
    for (int i = 0; i < poolSize; ++i) {
        pthread_join(threads[i], nullptr);
    }

    //TODO: maybe do this with a single allocation?
    for (FileBuffer b : compressData.compressedFrames) {
        buf.write_block(b.block, b.size());
        b.finalize();
    }

    buf.write<u8>(0x3B); //trailing marker
    timers.compress = get_time() - preCompress;

    float preWrite = get_time();
    //write data to file
    FILE * fp = fopen(path, "wb");
    assert(fp);
    fwrite(buf.block, buf.size(), 1, fp);
    fclose(fp);
    timers.write = get_time() - preWrite;

    //cleanup
    buf.finalize();
    free(cookedMemBlock);
    cookedFrames.finalize();
    free(meta.used);
    compressData.compressedFrames.finalize();

    timers.total = get_time() - preTotal;
    return timers;
}
