/* The mixer, to the sample.
 *
 * Nothing here reaches a speaker: WINRUN_NO_AUDIO is set so the device is
 * never opened, and w32_audio_mix is called the way the device would call it.
 * What is checked is the arithmetic a game's sound goes through -- format
 * conversion, rate, looping, the one-shot that ends, volume, pan, and the
 * clip when two sources add up to more than a sample can hold. It is integer
 * arithmetic on purpose, so these numbers are the same on every machine. */
#include "w32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
static void ok(int c, const char *what) { checks++; if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } }
static int all_zero(const int16_t *o, int n) { for (int i = 0; i < n; i++) if (o[i]) return 0; return 1; }

int main(void) {
    setenv("WINRUN_NO_AUDIO", "1", 1);
    int16_t mono[8] = { 1000, -1000, 2000, -2000, 3000, -3000, 4000, -4000 };
    w32_audio_src s; memset(&s, 0, sizeof s);
    s.mem = mono; s.size = sizeof mono; s.freq = 44100; s.channels = 1; s.bits = 16; s.playing = 1;
    int id = w32_audio_src_add(&s);
    ok(id >= 0, "a source can be added");

    int16_t out[2 * 16];
    w32_audio_mix(out, 8);
    int same = 1;
    for (int i = 0; i < 8; i++) if (out[2 * i] != mono[i] || out[2 * i + 1] != mono[i]) same = 0;
    ok(same, "16-bit mono at the output rate comes out unchanged on both channels");

    w32_audio_mix(out, 4);
    ok(all_zero(out, 8) && !w32_audio_src_playing(id), "a one-shot ends: silence after the last sample, and it says it stopped");

    s.looping = 1; w32_audio_src_set(id, &s);
    w32_audio_mix(out, 12);
    same = 1;
    for (int i = 0; i < 12; i++) if (out[2 * i] != mono[i % 8]) same = 0;
    ok(same && w32_audio_src_playing(id), "a looping source wraps to its start and keeps playing");

    s.vol_mB = -10000; w32_audio_src_set(id, &s);
    w32_audio_mix(out, 4);
    ok(all_zero(out, 8), "DSBVOLUME_MIN is silence");
    s.vol_mB = 0; s.pan_mB = -10000; w32_audio_src_set(id, &s);
    w32_audio_mix(out, 4);
    int left = 0, right = 1;
    for (int i = 0; i < 4; i++) { if (!out[2 * i]) left = 1; if (out[2 * i + 1]) right = 0; }
    ok(!left && right, "pan hard left keeps the left channel and silences the right");
    w32_audio_src_remove(id);

    /* half the output rate: every source frame is heard twice */
    int16_t slow[4] = { 100, 200, 300, 400 };
    memset(&s, 0, sizeof s);
    s.mem = slow; s.size = sizeof slow; s.freq = 22050; s.channels = 1; s.bits = 16; s.playing = 1;
    id = w32_audio_src_add(&s);
    w32_audio_mix(out, 8);
    same = 1;
    for (int i = 0; i < 8; i++) if (out[2 * i] != slow[i / 2]) same = 0;
    ok(same, "a 22050 Hz source plays each frame twice at 44100");
    w32_audio_src_remove(id);

    /* 8-bit stereo: unsigned, 128 is silence */
    uint8_t st8[4] = { 128, 255, 0, 128 };
    memset(&s, 0, sizeof s);
    s.mem = st8; s.size = sizeof st8; s.freq = 44100; s.channels = 2; s.bits = 8; s.playing = 1;
    id = w32_audio_src_add(&s);
    w32_audio_mix(out, 2);
    ok(out[0] == 0 && out[1] == 127 << 8 && out[2] == -32768 && out[3] == 0, "8-bit stereo converts: 128 is 0, 255 is loud, 0 is -32768");
    w32_audio_src_remove(id);

    /* two loud sources clip rather than wrap */
    int16_t loud[2] = { 30000, -30000 };
    memset(&s, 0, sizeof s);
    s.mem = loud; s.size = sizeof loud; s.freq = 44100; s.channels = 1; s.bits = 16; s.playing = 1;
    int a = w32_audio_src_add(&s), b = w32_audio_src_add(&s);
    w32_audio_mix(out, 2);
    ok(out[0] == 32767 && out[2] == -32768, "two sources at 30000 clip to the sample's limits");
    w32_audio_src_remove(a); w32_audio_src_remove(b);

    /* a WAV header is read, and an odd-length chunk is padded as RIFF says */
    uint8_t wav[12 + 8 + 16 + 8 + 4] = {0};
    memcpy(wav, "RIFF", 4); memcpy(wav + 8, "WAVE", 4); memcpy(wav + 12, "fmt ", 4);
    wav[16] = 16; wav[20] = 1; wav[22] = 1;                          /* PCM, mono */
    wav[24] = 0x44; wav[25] = 0xAC;                                  /* 44100 */
    wav[28] = 0x88; wav[29] = 0x58; wav[30] = 1;                     /* 88200 bytes/s */
    wav[32] = 2; wav[34] = 16;
    memcpy(wav + 36, "data", 4); wav[40] = 4; wav[44] = 0x10; wav[45] = 0x27; wav[46] = 0xF0; wav[47] = 0xD8;
    w32_audio_src ws;
    int parsed = w32_audio_parse_wav(wav, sizeof wav, &ws);
    ok(parsed && ws.freq == 44100 && ws.channels == 1 && ws.bits == 16 && ws.size == 4 && ws.mem == wav + 44 && !ws.is_float,
       "a PCM WAV header parses to the right format and data");
    ok(!w32_audio_parse_wav(wav, 20, &ws), "a truncated WAV is refused");

    w32_audio_close();
    printf("test_audio: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
