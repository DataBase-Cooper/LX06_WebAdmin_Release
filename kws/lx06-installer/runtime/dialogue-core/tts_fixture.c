/* Offline stock-ABI WAV/ring/identity fixture, without cloud or decoder exec. */
#include "tts.c"
#include <assert.h>

int main(void) {
    unsigned checks = 0;
    dc_tts_config config = {"/tmp/not-opened.sock", "/bin/busybox", 0};
    dc_tts *t = dc_tts_create(&config); assert(t); ++checks;
    t->last_tag = (dc_tag){7, 13, 19}; t->last_sequence = 5;
    struct tts_slot *s = &t->slots[0];
    s->used = 1; s->ready = 1; s->tag = t->last_tag; s->sequence = 5; t->active = 0;
    unsigned char wave[60] = {0};
    memcpy(wave, "RIFF", 4); ntts_put32(wave + 4, sizeof wave - 8);
    memcpy(wave + 8, "WAVEfmt ", 8); ntts_put32(wave + 16, 16);
    ntts_put16(wave + 20, 1); ntts_put16(wave + 22, 2);
    ntts_put32(wave + 24, 16000); ntts_put32(wave + 28, 64000);
    ntts_put16(wave + 32, 4); ntts_put16(wave + 34, 16);
    memcpy(wave + 36, "data", 4); ntts_put32(wave + 40, sizeof wave - 44);
    int16_t samples[] = {32767, 32767, -32768, -32768, 30000, -30000, 1000, 2000};
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; ++i) ntts_put16(wave + 44 + i * 2, (uint16_t)samples[i]);
    for (size_t i = 0; i < sizeof wave; ++i) { assert(!ingest_wave(t, wave + i, 1)); ++checks; }
    dc_tts_pcm_view view;
    assert(dc_tts_peek_pcm(t, &view) == 1 && view.bytes == 8 && view.rate == 16000); ++checks;
    assert(ntts_u16(view.data) == 32767 && ntts_u16(view.data + 2) == 32768 &&
           ntts_u16(view.data + 4) == 0 && ntts_u16(view.data + 6) == 1500); ++checks;
    assert(view.tag.session_epoch == 7 && view.tag.reply_generation == 13 && view.tag.utterance_id == 19); ++checks;
    assert(dc_tts_consume_pcm(t, (dc_tag){8, 13, 19}, 5, 8) == -1); ++checks;
    assert(dc_tts_consume_pcm(t, view.tag, 6, 8) == -1); ++checks;
    assert(dc_tts_consume_pcm(t, view.tag, 5, 7) == -1); ++checks;
    assert(dc_tts_consume_pcm(t, view.tag, 5, 8) == 0 && t->pcm_used == 0); ++checks;
    assert(!dc_tts_cancel(t, (dc_tag){7, 12, 19}, 100)); ++checks;
    assert(dc_tts_cancel(t, t->last_tag, 100) == 1); ++checks;
    assert(dc_tts_cancel(t, t->last_tag, 100) == 0 && dc_tts_slots(t) == 0); ++checks;
    assert(dc_tts_peek_pcm(t, &view) == 0); ++checks;
    size_t storage = dc_tts_storage_bytes(); assert(storage < 64 * 1024); ++checks;
    assert(dc_tts_destroy(t) == 0); ++checks;
    printf("{\"passed\":true,\"assertions\":%u,\"storage_bytes\":%zu,"
           "\"scope\":\"pure TTS WAV/ring/identity ABI\","
           "\"actual_audio\":false,\"provider_requests\":0}\n", checks, storage);
    return 0;
}
