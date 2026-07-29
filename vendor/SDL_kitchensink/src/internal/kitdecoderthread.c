#include <SDL_thread.h>
#include <SDL_timer.h>

#include "kitchensink2/internal/kitdecoder.h"
#include "kitchensink2/internal/kitdecoderthread.h"
#include "kitchensink2/internal/utils/kitlog.h"
#include "kitchensink2/kiterror.h"

static void Kit_ProcessPacket(Kit_DecoderThread *thread, bool *eof_received) {
    Kit_DecoderInputResult ret;
    bool is_eof;

    if(!Kit_BeginPacketBufferRead(thread->input, thread->scratch_packet, 100))
        return;

    // If a valid packet was found, first check whether it is a seek marker.
    // Seek packet is created in the demuxer, and is sent after avformat_seek_file() is called.
    if(thread->scratch_packet->stream_index ==
       KIT_PACKET_STREAM_INDEX_SEEK) {
        Kit_ClearDecoderBuffers(thread->decoder);
        goto finish;
    }
    is_eof = thread->scratch_packet->stream_index ==
        KIT_PACKET_STREAM_INDEX_EOF;

    // If valid packet was found and it is not a control packet, it must contain stream data.
    // Attempt to add it to the ffmpeg decoder internal queue. Note that the queue may be full, in which case
    // we try again later.
    ret = Kit_AddDecoderPacket(thread->decoder, is_eof ? NULL : thread->scratch_packet);
    if(ret == KIT_DEC_INPUT_RETRY)
        goto cancel;
    if(ret == KIT_DEC_INPUT_EOF || is_eof)
        *eof_received = true;

finish:
    Kit_FinishPacketBufferRead(thread->input);
    av_packet_unref(thread->scratch_packet);
    return;

cancel:
    Kit_CancelPacketBufferRead(thread->input);
    av_packet_unref(thread->scratch_packet);
    return;
}

static int Kit_DecodeMain(void *ptr) {
    Kit_DecoderThread *thread = ptr;
    bool eof_received = false;
    double pts;

    while(SDL_AtomicGet(&thread->run)) {
        Kit_ProcessPacket(thread, &eof_received);

        // Run the decoder. This will consume packets from the ffmpeg queue. We may need to call this multiple times,
        // since a single data packet might contain multiple frames.
        while(SDL_AtomicGet(&thread->run) &&
              Kit_RunDecoder(thread->decoder, &pts)) {
        }
        if(eof_received)
            break;
    }

    SDL_AtomicSet(&thread->run, 0);
    return 0;
}

Kit_DecoderThread *Kit_CreateDecoderThread(Kit_PacketBuffer *input, Kit_Decoder *decoder) {
    Kit_DecoderThread *decoder_thread;
    AVPacket *packet;

    if((packet = av_packet_alloc()) == NULL) {
        Kit_SetError("Unable to allocate decoder scratch packet");
        goto error_0;
    }
    if((decoder_thread = calloc(1, sizeof(Kit_DecoderThread))) == NULL) {
        Kit_SetError("Unable to allocate decoder thread");
        goto error_1;
    }

    decoder_thread->input = input;
    decoder_thread->decoder = decoder;
    decoder_thread->scratch_packet = packet;
    SDL_AtomicSet(&decoder_thread->run, 0);
    return decoder_thread;

error_1:
    av_packet_free(&packet);
error_0:
    return NULL;
}

void Kit_StartDecoderThread(Kit_DecoderThread *decoder_thread, const char *name) {
    if(!decoder_thread || decoder_thread->thread)
        return;
    SDL_AtomicSet(&decoder_thread->run, 1);
    decoder_thread->thread = SDL_CreateThread(Kit_DecodeMain, name, decoder_thread);
}

void Kit_StopDecoderThread(Kit_DecoderThread *decoder_thread) {
    if(!decoder_thread || !decoder_thread->thread)
        return;
    SDL_AtomicSet(&decoder_thread->run, 0);
}

void Kit_WaitDecoderThread(Kit_DecoderThread *decoder_thread) {
    if(!decoder_thread || !decoder_thread->thread)
        return;
    SDL_WaitThread(decoder_thread->thread, NULL);
    decoder_thread->thread = NULL;
}

bool Kit_IsDecoderThreadAlive(Kit_DecoderThread *decoder_thread) {
    return SDL_AtomicGet(&decoder_thread->run);
}

void Kit_CloseDecoderThread(Kit_DecoderThread **ref) {
    if(!ref || !*ref)
        return;
    Kit_DecoderThread *decoder_thread = *ref;
    Kit_StopDecoderThread(decoder_thread);
    Kit_WaitDecoderThread(decoder_thread);
    av_packet_free(&decoder_thread->scratch_packet);
    free(decoder_thread);
    *ref = NULL;
}
