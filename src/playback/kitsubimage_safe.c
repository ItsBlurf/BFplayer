/*
 * Hardened replacement for SDL_kitchensink's kitsubimage.c.
 *
 * Derived from SDL_kitchensink, Copyright (c) 2018 Tuomas Virtanen,
 * distributed under the MIT License. See THIRD_PARTY_NOTICES.md.
 *
 * The upstream object always copied 256 palette entries regardless of
 * AVSubtitleRect::nb_colors, interpreted FFmpeg's native RGB32 words as
 * SDL_Color byte fields, and grew four cache arrays with unchecked realloc.
 * Defining the same single external symbol here keeps the archive's unsafe
 * object unselected by the static linker.
 */

#include <SDL_render.h>
#include <SDL_surface.h>
#include <libavcodec/avcodec.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BFPLAYER_MAX_BITMAP_DIMENSION 16384
#define BFPLAYER_MAX_BITMAP_RECTS 4096U
#define BFPLAYER_MAX_CACHED_RECTS 1024U

typedef struct Kit_Decoder Kit_Decoder;
typedef struct Kit_PacketBuffer Kit_PacketBuffer;
typedef struct Kit_TextureAtlas Kit_TextureAtlas;
typedef struct Kit_SubtitleRenderer Kit_SubtitleRenderer;

typedef struct Kit_SubtitlePacket {
    double pts_start;
    double pts_end;
    int x;
    int y;
    bool clear;
    SDL_Surface *surface;
} Kit_SubtitlePacket;

typedef struct Kit_LibraryState {
    unsigned int init_flags;
    unsigned int thread_count;
    unsigned int font_hinting;
    unsigned int video_packet_buffer_size;
    unsigned int audio_packet_buffer_size;
    unsigned int subtitle_packet_buffer_size;
    unsigned int video_frame_buffer_size;
    unsigned int audio_frame_buffer_size;
    unsigned int subtitle_frame_buffer_size;
    unsigned int video_early_threshold;
    unsigned int video_late_threshold;
    unsigned int audio_early_threshold;
    unsigned int audio_late_threshold;
    void *libass_handle;
    void *ass_so_handle;
} Kit_LibraryState;

typedef void (*renderer_render_cb)(
    Kit_SubtitleRenderer *, void *, double, double, double);
typedef int (*renderer_get_data_cb)(
    Kit_SubtitleRenderer *, Kit_TextureAtlas *, SDL_Texture *, double);
typedef int (*renderer_get_raw_frames_cb)(
    Kit_SubtitleRenderer *, unsigned char ***, SDL_Rect **, SDL_Rect **, double);
typedef void (*renderer_set_size_cb)(Kit_SubtitleRenderer *, int, int);
typedef void (*renderer_simple_cb)(Kit_SubtitleRenderer *);

struct Kit_SubtitleRenderer {
    Kit_Decoder *decoder;
    void *userdata;
    renderer_render_cb render_cb;
    renderer_get_data_cb get_data_cb;
    renderer_set_size_cb set_size_cb;
    renderer_simple_cb flush_cb;
    renderer_simple_cb signal_cb;
    renderer_simple_cb close_cb;
    renderer_get_raw_frames_cb get_raw_frames_cb;
};

typedef void *(*buf_obj_alloc)(void);
typedef void (*buf_obj_unref)(void *);
typedef void (*buf_obj_free)(void **);
typedef void (*buf_obj_move)(void *, void *);
typedef bool (*buf_obj_ref)(void *, const void *);

extern Kit_LibraryState *Kit_GetLibraryState(void);
extern void Kit_SetError(const char *format, ...);
extern Kit_SubtitleRenderer *Kit_CreateSubtitleRenderer(
    Kit_Decoder *,
    renderer_render_cb,
    renderer_get_data_cb,
    renderer_get_raw_frames_cb,
    renderer_set_size_cb,
    renderer_simple_cb,
    renderer_simple_cb,
    renderer_simple_cb,
    void *);
extern void Kit_CloseSubtitleRenderer(Kit_SubtitleRenderer *);
extern Kit_PacketBuffer *Kit_CreatePacketBuffer(
    size_t,
    buf_obj_alloc,
    buf_obj_unref,
    buf_obj_free,
    buf_obj_move,
    buf_obj_ref);
extern void Kit_FreePacketBuffer(Kit_PacketBuffer **);
extern void Kit_SignalPacketBuffer(Kit_PacketBuffer *);
extern void Kit_FlushPacketBuffer(Kit_PacketBuffer *);
extern bool Kit_WritePacketBuffer(Kit_PacketBuffer *, void *);
extern bool Kit_ReadPacketBuffer(Kit_PacketBuffer *, void *, int);
extern Kit_SubtitlePacket *Kit_CreateSubtitlePacket(void);
extern void Kit_FreeSubtitlePacket(Kit_SubtitlePacket **);
extern void Kit_SetSubtitlePacketData(
    Kit_SubtitlePacket *, bool, double, double, int, int, SDL_Surface *);
extern void Kit_MoveSubtitlePacketRefs(Kit_SubtitlePacket *, Kit_SubtitlePacket *);
extern void Kit_DelSubtitlePacketRefs(Kit_SubtitlePacket *, bool);
extern void Kit_CheckAtlasTextureSize(Kit_TextureAtlas *, SDL_Texture *);
extern void Kit_ClearAtlasContent(Kit_TextureAtlas *);
extern int Kit_AddAtlasItem(
    Kit_TextureAtlas *, SDL_Texture *, const SDL_Surface *, const SDL_Rect *);

static void *packet_alloc(void) {
    return Kit_CreateSubtitlePacket();
}

static void packet_unref(void *packet) {
    Kit_DelSubtitlePacketRefs((Kit_SubtitlePacket *)packet, true);
}

static void packet_free(void **packet) {
    Kit_SubtitlePacket *subtitle_packet = *packet;
    Kit_FreeSubtitlePacket(&subtitle_packet);
    *packet = subtitle_packet;
}

static void packet_move(void *destination, void *source) {
    Kit_MoveSubtitlePacketRefs(
        (Kit_SubtitlePacket *)destination,
        (Kit_SubtitlePacket *)source);
}

static bool packet_ref(void *destination, const void *source) {
    (void)destination;
    (void)source;
    return false;
}

typedef struct Kit_ImageSubtitleRenderer {
    int video_w;
    int video_h;
    float scale_x;
    float scale_y;
    Kit_PacketBuffer *buffer;
    Kit_SubtitlePacket *in_packet;
    Kit_SubtitlePacket *out_packet;
    SDL_Surface **cached_surfaces;
    unsigned char **cached_items;
    SDL_Rect *cached_src_rects;
    SDL_Rect *cached_dst_rects;
    unsigned int cached_items_size;
    unsigned int cached_capacity;
} Kit_ImageSubtitleRenderer;

static bool rect_is_safe(const AVSubtitleRect *rect) {
    if(!rect || rect->type != SUBTITLE_BITMAP)
        return false;
    if(rect->w <= 0 || rect->h <= 0 ||
       rect->w > BFPLAYER_MAX_BITMAP_DIMENSION ||
       rect->h > BFPLAYER_MAX_BITMAP_DIMENSION ||
       rect->x < 0 || rect->y < 0 ||
       rect->x > BFPLAYER_MAX_BITMAP_DIMENSION - rect->w ||
       rect->y > BFPLAYER_MAX_BITMAP_DIMENSION - rect->h ||
       rect->linesize[0] < rect->w ||
       !rect->data[0] || !rect->data[1] ||
       rect->nb_colors <= 0 || rect->nb_colors > 256) {
        Kit_SetError("Invalid embedded bitmap subtitle rectangle");
        return false;
    }
    for(int y = 0; y < rect->h; ++y) {
        const uint8_t *row =
            rect->data[0] + (size_t)y * (size_t)rect->linesize[0];
        for(int x = 0; x < rect->w; ++x) {
            if(row[x] >= rect->nb_colors) {
                Kit_SetError("Embedded bitmap subtitle palette index is invalid");
                return false;
            }
        }
    }
    return true;
}

static SDL_Surface *convert_bitmap_rect(const AVSubtitleRect *rect) {
    SDL_Surface *indexed = SDL_CreateRGBSurfaceWithFormatFrom(
        rect->data[0],
        rect->w,
        rect->h,
        8,
        rect->linesize[0],
        SDL_PIXELFORMAT_INDEX8);
    if(!indexed) {
        Kit_SetError("Unable to create embedded subtitle indexed surface");
        return NULL;
    }

    SDL_Color palette[256];
    memset(palette, 0, sizeof(palette));
    for(int index = 0; index < rect->nb_colors; ++index) {
        uint32_t color = 0;
        memcpy(
            &color,
            rect->data[1] + (size_t)index * sizeof(color),
            sizeof(color));
        palette[index].r = (uint8_t)((color >> 16U) & 0xffU);
        palette[index].g = (uint8_t)((color >> 8U) & 0xffU);
        palette[index].b = (uint8_t)(color & 0xffU);
        palette[index].a = (uint8_t)((color >> 24U) & 0xffU);
    }
    if(SDL_SetPaletteColors(
           indexed->format->palette,
           palette,
           0,
           rect->nb_colors) != 0) {
        SDL_FreeSurface(indexed);
        Kit_SetError("Unable to set embedded subtitle palette");
        return NULL;
    }

    SDL_Surface *converted = SDL_CreateRGBSurfaceWithFormat(
        0, rect->w, rect->h, 32, SDL_PIXELFORMAT_RGBA32);
    if(!converted || SDL_BlitSurface(indexed, NULL, converted, NULL) != 0) {
        SDL_FreeSurface(indexed);
        SDL_FreeSurface(converted);
        Kit_SetError("Unable to convert embedded bitmap subtitle");
        return NULL;
    }
    SDL_FreeSurface(indexed);
    return converted;
}

static void render_image(
    Kit_SubtitleRenderer *renderer,
    void *source,
    double pts,
    double start,
    double end) {
    if(!renderer || !renderer->userdata || !source)
        return;
    Kit_ImageSubtitleRenderer *image_renderer = renderer->userdata;
    const AVSubtitle *subtitle = source;
    const double start_pts = pts + start;
    const double end_pts = pts + end;

    if(subtitle->num_rects == 0) {
        Kit_SetSubtitlePacketData(
            image_renderer->in_packet,
            true,
            start_pts,
            end_pts,
            0,
            0,
            NULL);
        Kit_WritePacketBuffer(
            image_renderer->buffer, image_renderer->in_packet);
        return;
    }
    if(subtitle->num_rects > BFPLAYER_MAX_BITMAP_RECTS) {
        Kit_SetError("Embedded bitmap subtitle has too many rectangles");
        return;
    }

    for(unsigned int index = 0; index < subtitle->num_rects; ++index) {
        const AVSubtitleRect *rect = subtitle->rects[index];
        if(!rect || rect->type != SUBTITLE_BITMAP)
            continue;
        if(!rect_is_safe(rect))
            continue;
        SDL_Surface *converted = convert_bitmap_rect(rect);
        if(!converted)
            continue;
        Kit_SetSubtitlePacketData(
            image_renderer->in_packet,
            false,
            start_pts,
            end_pts,
            rect->x,
            rect->y,
            converted);
        if(!Kit_WritePacketBuffer(
                image_renderer->buffer, image_renderer->in_packet)) {
            Kit_DelSubtitlePacketRefs(image_renderer->in_packet, true);
            return;
        }
    }
}

static bool process_to_atlas(
    Kit_ImageSubtitleRenderer *renderer,
    Kit_TextureAtlas *atlas,
    SDL_Texture *texture,
    double current_pts) {
    if(!renderer->out_packet->surface && !renderer->out_packet->clear) {
        Kit_DelSubtitlePacketRefs(renderer->out_packet, true);
        return false;
    }
    if(renderer->out_packet->pts_end < current_pts) {
        Kit_DelSubtitlePacketRefs(renderer->out_packet, true);
        return false;
    }
    if(renderer->out_packet->clear)
        Kit_ClearAtlasContent(atlas);
    if(renderer->out_packet->surface) {
        SDL_Rect target;
        target.x = (int)(renderer->out_packet->x * renderer->scale_x);
        target.y = (int)(renderer->out_packet->y * renderer->scale_y);
        target.w =
            (int)(renderer->out_packet->surface->w * renderer->scale_x);
        target.h =
            (int)(renderer->out_packet->surface->h * renderer->scale_y);
        Kit_AddAtlasItem(
            atlas, texture, renderer->out_packet->surface, &target);
    }
    Kit_DelSubtitlePacketRefs(renderer->out_packet, true);
    return true;
}

static int get_image_data(
    Kit_SubtitleRenderer *renderer,
    Kit_TextureAtlas *atlas,
    SDL_Texture *texture,
    double current_pts) {
    Kit_ImageSubtitleRenderer *image_renderer = renderer->userdata;
    Kit_CheckAtlasTextureSize(atlas, texture);
    process_to_atlas(image_renderer, atlas, texture, current_pts);
    while(Kit_ReadPacketBuffer(
              image_renderer->buffer, image_renderer->out_packet, 0)) {
        if(!process_to_atlas(
                image_renderer, atlas, texture, current_pts))
            break;
    }
    return 0;
}

static void clear_cache(Kit_ImageSubtitleRenderer *renderer) {
    for(unsigned int index = 0;
        index < renderer->cached_items_size;
        ++index) {
        SDL_FreeSurface(renderer->cached_surfaces[index]);
        renderer->cached_surfaces[index] = NULL;
        renderer->cached_items[index] = NULL;
    }
    renderer->cached_items_size = 0;
}

static bool process_to_cache(
    Kit_ImageSubtitleRenderer *renderer,
    double current_pts) {
    if(!renderer->out_packet->surface && !renderer->out_packet->clear) {
        Kit_DelSubtitlePacketRefs(renderer->out_packet, true);
        return false;
    }
    if(renderer->out_packet->pts_end < current_pts) {
        Kit_DelSubtitlePacketRefs(renderer->out_packet, true);
        return false;
    }
    if(renderer->out_packet->clear)
        clear_cache(renderer);
    if(renderer->out_packet->surface) {
        if(renderer->cached_items_size >= renderer->cached_capacity) {
            Kit_SetError("Embedded subtitle cache reached its safe limit");
            Kit_DelSubtitlePacketRefs(renderer->out_packet, true);
            return false;
        }
        const unsigned int index = renderer->cached_items_size++;
        renderer->cached_surfaces[index] = renderer->out_packet->surface;
        renderer->cached_items[index] = renderer->out_packet->surface->pixels;
        renderer->cached_src_rects[index] = (SDL_Rect){
            0,
            0,
            renderer->out_packet->surface->w,
            renderer->out_packet->surface->h};
        renderer->cached_dst_rects[index] = (SDL_Rect){
            (int)(renderer->out_packet->x * renderer->scale_x),
            (int)(renderer->out_packet->y * renderer->scale_y),
            (int)(renderer->out_packet->surface->w * renderer->scale_x),
            (int)(renderer->out_packet->surface->h * renderer->scale_y)};
    }
    Kit_DelSubtitlePacketRefs(renderer->out_packet, false);
    return true;
}

static int get_raw_frames(
    Kit_SubtitleRenderer *renderer,
    unsigned char ***frames,
    SDL_Rect **sources,
    SDL_Rect **targets,
    double current_pts) {
    Kit_ImageSubtitleRenderer *image_renderer = renderer->userdata;
    process_to_cache(image_renderer, current_pts);
    while(Kit_ReadPacketBuffer(
              image_renderer->buffer, image_renderer->out_packet, 0)) {
        if(!process_to_cache(image_renderer, current_pts))
            break;
    }
    *frames = image_renderer->cached_items;
    *sources = image_renderer->cached_src_rects;
    *targets = image_renderer->cached_dst_rects;
    return (int)image_renderer->cached_items_size;
}

static void set_image_size(
    Kit_SubtitleRenderer *renderer, int width, int height) {
    Kit_ImageSubtitleRenderer *image_renderer = renderer->userdata;
    image_renderer->scale_x =
        (float)width / (float)image_renderer->video_w;
    image_renderer->scale_y =
        (float)height / (float)image_renderer->video_h;
}

static void flush_image(Kit_SubtitleRenderer *renderer) {
    Kit_ImageSubtitleRenderer *image_renderer = renderer->userdata;
    Kit_FlushPacketBuffer(image_renderer->buffer);
}

static void signal_image(Kit_SubtitleRenderer *renderer) {
    Kit_ImageSubtitleRenderer *image_renderer = renderer->userdata;
    Kit_SignalPacketBuffer(image_renderer->buffer);
}

static void close_image(Kit_SubtitleRenderer *renderer) {
    if(!renderer || !renderer->userdata)
        return;
    Kit_ImageSubtitleRenderer *image_renderer = renderer->userdata;
    if(image_renderer->buffer)
        Kit_FreePacketBuffer(&image_renderer->buffer);
    if(image_renderer->in_packet)
        Kit_FreeSubtitlePacket(&image_renderer->in_packet);
    if(image_renderer->out_packet)
        Kit_FreeSubtitlePacket(&image_renderer->out_packet);
    clear_cache(image_renderer);
    free(image_renderer->cached_items);
    free(image_renderer->cached_surfaces);
    free(image_renderer->cached_src_rects);
    free(image_renderer->cached_dst_rects);
    free(image_renderer);
    renderer->userdata = NULL;
}

Kit_SubtitleRenderer *Kit_CreateImageSubtitleRenderer(
    Kit_Decoder *decoder,
    int video_w,
    int video_h,
    int screen_w,
    int screen_h) {
    if(!decoder || video_w <= 0 || video_h <= 0 ||
       screen_w <= 0 || screen_h <= 0) {
        Kit_SetError("Invalid embedded subtitle renderer dimensions");
        return NULL;
    }
    Kit_LibraryState *state = Kit_GetLibraryState();
    if(!state || state->subtitle_frame_buffer_size == 0) {
        Kit_SetError("Invalid Kitchensink subtitle buffer configuration");
        return NULL;
    }

    Kit_ImageSubtitleRenderer *image_renderer =
        calloc(1, sizeof(Kit_ImageSubtitleRenderer));
    if(!image_renderer) {
        Kit_SetError("Unable to allocate embedded subtitle renderer");
        return NULL;
    }
    image_renderer->cached_capacity = BFPLAYER_MAX_CACHED_RECTS;
    image_renderer->cached_items =
        calloc(image_renderer->cached_capacity, sizeof(unsigned char *));
    image_renderer->cached_surfaces =
        calloc(image_renderer->cached_capacity, sizeof(SDL_Surface *));
    image_renderer->cached_src_rects =
        calloc(image_renderer->cached_capacity, sizeof(SDL_Rect));
    image_renderer->cached_dst_rects =
        calloc(image_renderer->cached_capacity, sizeof(SDL_Rect));
    if(!image_renderer->cached_items ||
       !image_renderer->cached_surfaces ||
       !image_renderer->cached_src_rects ||
       !image_renderer->cached_dst_rects) {
        free(image_renderer->cached_items);
        free(image_renderer->cached_surfaces);
        free(image_renderer->cached_src_rects);
        free(image_renderer->cached_dst_rects);
        free(image_renderer);
        Kit_SetError("Unable to allocate embedded subtitle cache");
        return NULL;
    }

    image_renderer->video_w = video_w;
    image_renderer->video_h = video_h;
    image_renderer->scale_x = (float)screen_w / (float)video_w;
    image_renderer->scale_y = (float)screen_h / (float)video_h;

    Kit_SubtitleRenderer *renderer = Kit_CreateSubtitleRenderer(
        decoder,
        render_image,
        get_image_data,
        get_raw_frames,
        set_image_size,
        flush_image,
        signal_image,
        close_image,
        image_renderer);
    if(!renderer) {
        free(image_renderer->cached_items);
        free(image_renderer->cached_surfaces);
        free(image_renderer->cached_src_rects);
        free(image_renderer->cached_dst_rects);
        free(image_renderer);
        return NULL;
    }

    image_renderer->buffer = Kit_CreatePacketBuffer(
        state->subtitle_frame_buffer_size,
        packet_alloc,
        packet_unref,
        packet_free,
        packet_move,
        packet_ref);
    if(!image_renderer->buffer) {
        Kit_CloseSubtitleRenderer(renderer);
        return NULL;
    }
    image_renderer->in_packet = Kit_CreateSubtitlePacket();
    if(!image_renderer->in_packet) {
        Kit_CloseSubtitleRenderer(renderer);
        return NULL;
    }
    image_renderer->out_packet = Kit_CreateSubtitlePacket();
    if(!image_renderer->out_packet) {
        Kit_CloseSubtitleRenderer(renderer);
        return NULL;
    }
    return renderer;
}
