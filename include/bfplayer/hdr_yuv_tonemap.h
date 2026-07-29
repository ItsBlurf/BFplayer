#ifndef BFPLAYER_HDR_YUV_TONEMAP_H
#define BFPLAYER_HDR_YUV_TONEMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BFPLAYER_HDR_CHROMA_LEVELS 17
#define BFPLAYER_HDR_LUMA_LEVELS 256
#define BFPLAYER_HDR_YUV_ENTRY_COUNT \
    (BFPLAYER_HDR_CHROMA_LEVELS * BFPLAYER_HDR_CHROMA_LEVELS * \
     BFPLAYER_HDR_LUMA_LEVELS)

typedef enum BfplayerHdrTransfer {
    BFPLAYER_HDR_TRANSFER_PQ = 1,
    BFPLAYER_HDR_TRANSFER_HLG = 2,
} BfplayerHdrTransfer;

typedef struct BfplayerHdrYuvConfig {
    BfplayerHdrTransfer transfer;
    int input_full_range;
    int input_bt2020;
    double source_peak_nits;
    double target_peak_nits;
} BfplayerHdrYuvConfig;

typedef struct BfplayerHdrYuvEntry {
    uint32_t packed;
} BfplayerHdrYuvEntry;

typedef struct BfplayerHdrYuvLut {
    BfplayerHdrYuvConfig config;
    BfplayerHdrYuvEntry entries[BFPLAYER_HDR_YUV_ENTRY_COUNT];
} BfplayerHdrYuvLut;

typedef struct BfplayerHdrWorkerPool BfplayerHdrWorkerPool;

/*
 * Builds a compact YUV lookup table for software HDR-to-SDR conversion.
 * Input is BT.2020 or BT.709 PQ/HLG YUV. Output is always limited-range
 * BT.709 SDR so SDL can present it with its BT.709 conversion path.
 */
int bfplayer_build_hdr_yuv420_lut(
    const BfplayerHdrYuvConfig* config,
    BfplayerHdrYuvLut* lut);

/*
 * Applies the table in place to one 8-bit planar YUV420 frame. The function
 * does not allocate another frame and leaves row padding untouched.
 */
int bfplayer_apply_hdr_yuv420_lut(
    const BfplayerHdrYuvLut* lut,
    uint8_t* y_plane,
    int y_stride,
    uint8_t* u_plane,
    int u_stride,
    uint8_t* v_plane,
    int v_stride,
    int width,
    int height);

/*
 * Creates persistent workers used by the PS5 decoder. worker_count is the
 * number of background workers; the calling decoder thread also participates.
 */
BfplayerHdrWorkerPool* bfplayer_create_hdr_worker_pool(
    unsigned int worker_count);

void bfplayer_destroy_hdr_worker_pool(
    BfplayerHdrWorkerPool* pool);

unsigned int bfplayer_hdr_worker_count(
    const BfplayerHdrWorkerPool* pool);

int bfplayer_apply_hdr_yuv420_lut_parallel(
    BfplayerHdrWorkerPool* pool,
    const BfplayerHdrYuvLut* lut,
    uint8_t* y_plane,
    int y_stride,
    uint8_t* u_plane,
    int u_stride,
    uint8_t* v_plane,
    int v_stride,
    int width,
    int height);

#ifdef __cplusplus
}
#endif

#endif
