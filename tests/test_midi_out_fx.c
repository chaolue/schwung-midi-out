/*
 * midi-out MIDI FX: what reaches USB-A, and what the chain gets back.
 *
 * Runs natively (tests/run.sh); the module has no device dependencies.
 *
 * The send is host->midi_send_external, faked here as a capture buffer that
 * can be told to REFUSE — the shim ring drops-newest when full, and the case
 * worth pinning is a refused note-off still arriving later, in order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "midi_fx_api_v1.h"
#include "plugin_api_v1.h"

extern midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host);

#define CAP 256
static uint8_t sent[CAP][4];
static int sent_n = 0;
static int refuse = 0;      /* while non-zero, every send returns 0 */
static int attempts = 0;

static int fake_send_external(const uint8_t *msg, int len) {
    attempts++;
    if (refuse || len != 4 || sent_n >= CAP) return 0;
    memcpy(sent[sent_n++], msg, 4);
    return len;
}

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); failures++; } } while (0)

static midi_fx_api_v1_t *api;

static int run(void *inst, uint8_t s, uint8_t d1, uint8_t d2, int len,
               uint8_t out[][3], int *out_lens) {
    const uint8_t in[3] = { s, d1, d2 };
    return api->process_midi(inst, in, len, out, out_lens, MIDI_FX_MAX_OUT_MSGS);
}

static void play(void *inst, uint8_t s, uint8_t d1, uint8_t d2) {
    uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
    int lens[MIDI_FX_MAX_OUT_MSGS];
    run(inst, s, d1, d2, 3, out, lens);
}

static void tick(void *inst) {
    uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
    int lens[MIDI_FX_MAX_OUT_MSGS];
    int n = api->tick(inst, 128, 44100, out, lens, MIDI_FX_MAX_OUT_MSGS);
    CHECK(n == 0, "tick generated %d messages; it only retries the send", n);
}

static int pkt_is(int i, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    return i < sent_n && sent[i][0] == b0 && sent[i][1] == b1 &&
           sent[i][2] == b2 && sent[i][3] == b3;
}

static void reset_capture(void) { sent_n = 0; attempts = 0; refuse = 0; }

int main(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.midi_send_external = fake_send_external;

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->process_midi || !api->tick ||
        !api->set_param || !api->get_param || !api->destroy_instance) {
        fprintf(stderr, "FAIL: API incomplete\n");
        return 1;
    }
    /* The module must keep the FUNCTION, not this struct: a chain hands it the
     * address of a host_api copy that can be freed while another slot still
     * runs the module. Scribble over ours to prove nothing reads it later. */
    memset(&host, 0xA5, sizeof(host));

    char buf[512];
    void *inst = api->create_instance(".", NULL);
    CHECK(inst != NULL, "create_instance returned NULL");

    /* --- defaults ----------------------------------------------------- */
    api->get_param(inst, "channel", buf, sizeof(buf));
    CHECK(strcmp(buf, "1") == 0, "default channel is '%s', want 1", buf);
    CHECK(api->get_param(inst, "chain_params", buf, sizeof(buf)) > 0 &&
          strstr(buf, "\"key\":\"channel\"") && strstr(buf, "\"type\":\"int\""),
          "chain_params does not declare an int channel: %s", buf);

    /* --- re-channel and pass through ----------------------------------- */
    api->set_param(inst, "channel", "5");
    reset_capture();
    {
        uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
        int lens[MIDI_FX_MAX_OUT_MSGS];
        int n = run(inst, 0x92, 60, 100, 3, out, lens);   /* note-on, ch 3 */
        CHECK(n == 1 && out[0][0] == 0x92 && out[0][1] == 60 && out[0][2] == 100 && lens[0] == 3,
              "note-on not passed down the chain unchanged");
    }
    CHECK(sent_n == 1 && pkt_is(0, 0x29, 0x94, 60, 100),
          "note-on should go out cable 2 / CIN 9 on ch 5 (got %d pkts %02x %02x %02x %02x)",
          sent_n, sent[0][0], sent[0][1], sent[0][2], sent[0][3]);

    play(inst, 0xB0, 74, 33);               /* CC */
    play(inst, 0xE7, 0x00, 0x40);           /* pitch bend */
    play(inst, 0x82, 60, 0);                /* note-off */
    CHECK(pkt_is(1, 0x2B, 0xB4, 74, 33), "CC not re-channelled with CIN 0xB");
    CHECK(pkt_is(2, 0x2E, 0xE4, 0x00, 0x40), "bend not re-channelled with CIN 0xE");
    CHECK(pkt_is(3, 0x28, 0x84, 60, 0), "note-off not re-channelled with CIN 0x8");

    /* Two-byte messages carry a zero third byte, whatever the caller left. */
    {
        reset_capture();
        uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
        int lens[MIDI_FX_MAX_OUT_MSGS];
        const uint8_t pc[2] = { 0xC1, 7 };
        int n = api->process_midi(inst, pc, 2, out, lens, MIDI_FX_MAX_OUT_MSGS);
        CHECK(n == 1 && lens[0] == 2 && out[0][0] == 0xC1 && out[0][1] == 7,
              "program change not passed through with its length");
        CHECK(sent_n == 1 && pkt_is(0, 0x2C, 0xC4, 7, 0), "program change not sent as CIN 0xC");
    }

    /* --- realtime clock: passed through, never sent ---------------------- */
    {
        reset_capture();
        uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
        int lens[MIDI_FX_MAX_OUT_MSGS];
        const uint8_t clk[1] = { 0xF8 };
        int n = api->process_midi(inst, clk, 1, out, lens, MIDI_FX_MAX_OUT_MSGS);
        CHECK(n == 1 && lens[0] == 1 && out[0][0] == 0xF8, "clock not passed through");
        const uint8_t start[1] = { 0xFA };
        api->process_midi(inst, start, 1, out, lens, MIDI_FX_MAX_OUT_MSGS);
        CHECK(sent_n == 0, "realtime reached USB-A (%d pkts); Move's Clock Out owns that", sent_n);
        /* Three bytes long, so only the status check stands between this and
         * a re-channelled 0xF4 on the wire. */
        n = run(inst, 0xF2, 0x10, 0x00, 3, out, lens);
        CHECK(n == 1 && out[0][0] == 0xF2, "song position not passed through");
        CHECK(sent_n == 0, "a system common message was sent (%d pkts)", sent_n);
    }

    /* --- nothing jumps the queue ----------------------------------------- */
    reset_capture();
    play(inst, 0x90, 66, 90);
    refuse = 1;
    play(inst, 0x80, 66, 0);                /* refused, queued */
    refuse = 0;                             /* ring drains before the next tick */
    play(inst, 0x90, 66, 90);               /* the retrigger must follow the release */
    CHECK(sent_n == 3 && pkt_is(1, 0x28, 0x84, 66, 0) && pkt_is(2, 0x29, 0x94, 66, 90),
          "a new packet overtook a queued release: the synth gets on, on, off");
    play(inst, 0x80, 66, 0);

    /* --- a refused note-off is retried, in order ------------------------ */
    reset_capture();
    play(inst, 0x90, 62, 90);
    refuse = 1;
    play(inst, 0x80, 62, 0);                /* refused by the ring */
    play(inst, 0x90, 64, 90);               /* must wait behind it */
    CHECK(sent_n == 1, "nothing should get past a full ring (sent %d)", sent_n);
    tick(inst);
    CHECK(sent_n == 1, "still full: nothing should go out");
    refuse = 0;
    tick(inst);
    CHECK(sent_n == 3 && pkt_is(1, 0x28, 0x84, 62, 0) && pkt_is(2, 0x29, 0x94, 64, 90),
          "queued packets not delivered in order once the ring had room");
    play(inst, 0x80, 64, 0);

    /* --- note-offs follow their note-on's channel ----------------------- */
    reset_capture();
    play(inst, 0x90, 48, 100);              /* out on ch 5 */
    api->set_param(inst, "channel", "9");
    play(inst, 0x90, 50, 100);              /* out on ch 9 */
    play(inst, 0x80, 48, 0);                /* must release on 5, not 9 */
    play(inst, 0x90, 50, 0);                /* vel-0 note-on is a release, on 9 */
    CHECK(pkt_is(2, 0x28, 0x84, 48, 0), "held note released on the new channel, stranding it");
    CHECK(pkt_is(3, 0x29, 0x98, 50, 0), "velocity-0 release not on its note-on's channel");

    /* A release for a note never sent still goes out, on the current channel. */
    play(inst, 0x80, 99, 0);
    CHECK(pkt_is(4, 0x28, 0x88, 99, 0), "orphan note-off dropped instead of sent");

    /* --- sustain is lifted where it was put down ------------------------ */
    reset_capture();
    play(inst, 0xB0, 64, 127);              /* down on ch 9 */
    api->set_param(inst, "channel", "2");
    play(inst, 0xB0, 64, 0);                /* up: on 2, and on 9 */
    CHECK(sent_n == 3 && pkt_is(1, 0x2B, 0xB1, 64, 0) && pkt_is(2, 0x2B, 0xB8, 64, 0),
          "sustain left down on the old channel (%d pkts)", sent_n);

    /* --- channel parsing ------------------------------------------------ */
    api->set_param(inst, "channel", "12.000000");
    api->get_param(inst, "channel", buf, sizeof(buf));
    CHECK(strcmp(buf, "12") == 0, "knob-style float write gave %s", buf);
    api->set_param(inst, "channel", "99");
    api->get_param(inst, "channel", buf, sizeof(buf));
    CHECK(strcmp(buf, "16") == 0, "channel not clamped to 16 (%s)", buf);
    api->set_param(inst, "channel", "0");
    api->get_param(inst, "channel", buf, sizeof(buf));
    CHECK(strcmp(buf, "1") == 0, "channel not clamped to 1 (%s)", buf);
    api->set_param(inst, "channel", "garbage");
    api->get_param(inst, "channel", buf, sizeof(buf));
    CHECK(strcmp(buf, "1") == 0, "a non-number moved the channel (%s)", buf);

    /* --- state round trip ----------------------------------------------- */
    api->set_param(inst, "channel", "7");
    api->get_param(inst, "state", buf, sizeof(buf));
    CHECK(strcmp(buf, "{\"channel\":7}") == 0, "state is %s", buf);
    void *other = api->create_instance(".", NULL);
    api->set_param(other, "state", buf);
    api->get_param(other, "channel", buf, sizeof(buf));
    CHECK(strcmp(buf, "7") == 0, "state did not restore the channel (%s)", buf);
    api->destroy_instance(other);

    /* --- queue full: a release evicts a note-on, never the other way ---- */
    reset_capture();
    api->set_param(inst, "channel", "1");
    play(inst, 0x90, 70, 100);              /* sounding, sent */
    refuse = 1;
    for (int i = 0; i < 80; i++) play(inst, 0xB0, 1, (uint8_t)(i & 0x7F));
    play(inst, 0x80, 70, 0);                /* queue is full of CCs */
    refuse = 0;
    for (int i = 0; i < 4; i++) tick(inst);
    int found = 0;
    for (int i = 0; i < sent_n; i++) if (pkt_is(i, 0x28, 0x80, 70, 0)) found = 1;
    CHECK(found, "note-off lost to a full queue of CCs");
    api->get_param(inst, "dropped", buf, sizeof(buf));
    CHECK(atoi(buf) > 0, "drops not counted (%s)", buf);

    /* --- destroy releases what is still sounding ------------------------ */
    reset_capture();
    api->set_param(inst, "channel", "3");
    play(inst, 0x90, 40, 100);
    play(inst, 0xB0, 64, 127);
    api->set_param(inst, "channel", "4");
    play(inst, 0x90, 41, 100);
    int before = sent_n;
    api->destroy_instance(inst);
    int off40 = 0, off41 = 0, ped = 0;
    for (int i = before; i < sent_n; i++) {
        if (pkt_is(i, 0x28, 0x82, 40, 0)) off40 = 1;
        if (pkt_is(i, 0x28, 0x83, 41, 0)) off41 = 1;
        if (pkt_is(i, 0x2B, 0xB2, 64, 0)) ped = 1;
    }
    CHECK(off40 && off41 && ped, "destroy left notes or the pedal down on the external synth");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: midi-out MIDI FX\n");
    return 0;
}
