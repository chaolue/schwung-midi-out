/*
 * MIDI Out — a chain MIDI FX that sends what reaches it out of USB-A.
 *
 * Put it after any other MIDI FX in a slot and everything those produce —
 * played notes, chords, arpeggiator steps, CCs, bend — goes out of the
 * external MIDI port on one channel the user picks.
 *
 * THRU (default On) decides whether it is a TAP or a SINK. On, every message
 * also continues down the chain unchanged, so a synth (or a further MIDI FX)
 * after it keeps playing. Off, what goes out of USB-A stops here, so only the
 * external synth plays — except what the slot's synth is still OWED: the
 * release of a note or sustain pedal that reached it while Thru was on.
 * Without that, switching Thru off with a key held would leave the slot's
 * synth sounding the note forever. System messages pass either way: they are
 * never sent out of USB-A, so blocking them would drop them entirely, and
 * clock is how a synth after this module keeps tempo.
 *
 * Because process_midi() is called for every message that leaves the stage
 * before it — including what an upstream arpeggiator generates from tick(),
 * which Schwung's chain runs through the remaining stages
 * (v2_run_midi_fx_from in src/modules/chain/dsp/chain_midi.c) —
 * this sees the whole post-FX stream without a tick() of its own producing
 * anything.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS FORWARDED: channel voice messages only (0x80-0xEF), re-channelled.
 *
 * Realtime clock reaches MIDI FX too (len 1, for the arpeggiator's sync), and
 * is passed through but never sent: Move's own MIDI Clock Out already puts it
 * on USB-A, and a second copy would double the external device's tempo.
 * SysEx never reaches a MIDI FX unless it declares wants_sysex, and this one
 * does not.
 *
 * ---------------------------------------------------------------------------
 * host->midi_send_external IS NOT A DELIVERY GUARANTEE (see plugin_api_v1.h).
 *
 * It pushes onto the shim's 64-packet ROUTE_EXTERNAL ring, which drops the
 * NEWEST packet when full and shares its 20-slot-per-block drain with Move's
 * own output. A dropped note-on is a missed note; a dropped note-OFF is a note
 * stuck on the external synth until somebody pulls the cable. So a refused
 * packet is queued here and retried from tick(), which the chain calls on
 * every block, idle ones included. Order is preserved — nothing jumps the
 * queue — and if the queue itself fills, a release (note-off, sustain up,
 * all-notes-off) evicts the oldest non-release rather than being lost.
 *
 * NOTE-OFFS FOLLOW THEIR NOTE-ON'S CHANNEL, not the current setting. Change the
 * channel while a key is held and the release still goes where the note went;
 * otherwise every channel change mid-phrase strands whatever was sounding. The
 * same holds for a sustain pedal left down.
 *
 * ---------------------------------------------------------------------------
 * REALTIME: every entry point here IS the SPI callback (see the contract at the
 * top of plugin_api_v1.h). Nothing allocates outside create, nothing logs,
 * nothing touches a file, nothing locks. midi_send_external is a lock-free ring
 * push documented safe from exactly this context.
 *
 * THE SEND POINTER IS COPIED, NOT THE HOST POINTER. A chain hands each MIDI FX
 * the address of its OWN host_api copy (chain_instance_t::subplugin_host_api),
 * and this .so is shared by every slot that loads it, so a stored `host` names
 * whichever slot initialised it last — and dangles if that chain is freed while
 * another slot still runs this module. The function it points at is the
 * shim's and lives for the process, so that is what is kept.
 *
 * ---------------------------------------------------------------------------
 * THE HEADERS ARE VENDORED from Schwung's src/host (see README.md). A module's
 * copy of host_api_v1_t that drifts from the host's reads someone else's
 * memory, so the one offset this module reads is pinned below: if a future
 * header copy moves it, the build fails instead of the device.
 */

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "plugin_api_v1.h"
#include "midi_fx_api_v1.h"

/* host_api_v1_t: api_version, sample_rate, frames_per_block, mapped_memory,
 * audio_out_offset, audio_in_offset, log, midi_send_internal, then this. */
#if defined(__LP64__)
_Static_assert(offsetof(host_api_v1_t, midi_send_external) == 48,
               "midi_send_external moved: the vendored plugin_api_v1.h no "
               "longer matches the Schwung host ABI");
#endif

#define MO_CABLE_EXTERNAL 2
#define MO_PENDING_MAX    64      /* matches the shim ring it backs up */
#define MO_CHANNEL_MIN    1
#define MO_CHANNEL_MAX    16
#define MO_CC_SUSTAIN     64

typedef int (*mo_send_fn)(const uint8_t *msg, int len);
static mo_send_fn g_send_external = NULL;

typedef struct {
    int     channel;                 /* 0-15; shown and stored as 1-16 */
    int     thru;                    /* 1: also pass down the chain (default) */

    /* What went down the chain while Thru was on and has not been released
     * yet, per INPUT channel and pitch: the releases Thru Off must still let
     * through. Input channel, because pass-through is unchanged. */
    uint8_t thru_refs[16][128];
    uint16_t thru_sustain;           /* bit per input channel, pedal passed down */

    /* Note-ons sent minus note-offs sent, per output channel and pitch. A
     * release is routed to the channel its note-on went to, so a channel
     * change while a key is held cannot strand the note. */
    uint8_t refs[16][128];
    uint16_t sustain_down;           /* bit per channel we left the pedal down on */

    /* Packets the shim ring refused, oldest first, retried every block. */
    uint8_t pending[MO_PENDING_MAX][4];
    int     pending_count;
    int     dropped;                 /* lost even after queueing; diagnostic */
} midi_out_t;

/* chain_params is where the knob grid takes its metadata from — module.json's
 * ui_hierarchy declares structure, not types (see sysex_probe.c). */
static const char CHAIN_PARAMS_JSON[] =
    "["
    "{\"key\":\"channel\",\"name\":\"MIDI Channel\",\"short_name\":\"Chan\","
      "\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1,\"default\":1},"
    "{\"key\":\"thru\",\"name\":\"Pass Thru\",\"short_name\":\"Thru\","
      "\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"options_as_string\":true,"
      "\"default\":1}"
    "]";

/* ---- Queue --------------------------------------------------------------- */

/* A packet that ends something. Losing one of these leaves the external synth
 * sounding, so they outrank everything else for a place in the queue. */
static int mo_is_release(const uint8_t pkt[4]) {
    uint8_t type = pkt[1] & 0xF0;
    if (type == 0x80) return 1;
    if (type == 0x90 && pkt[3] == 0) return 1;
    if (type == 0xB0) {
        if (pkt[2] == MO_CC_SUSTAIN && pkt[3] < 64) return 1;
        if (pkt[2] == 120 || pkt[2] == 123) return 1;   /* all sound / notes off */
    }
    return 0;
}

static void mo_flush(midi_out_t *m) {
    if (!g_send_external || m->pending_count == 0) return;
    int sent = 0;
    while (sent < m->pending_count && g_send_external(m->pending[sent], 4) > 0) {
        sent++;
    }
    if (sent == 0) return;
    m->pending_count -= sent;
    if (m->pending_count > 0) {
        memmove(m->pending[0], m->pending[sent], (size_t)m->pending_count * 4);
    }
}

static void mo_enqueue(midi_out_t *m, const uint8_t pkt[4]) {
    if (m->pending_count >= MO_PENDING_MAX) {
        if (!mo_is_release(pkt)) { m->dropped++; return; }
        /* Make room by evicting the oldest packet that is not itself a
         * release. Evicting a queued note-on is safe: its note-off will still
         * go out, and an unmatched note-off is a no-op on any synth. */
        int victim = -1;
        for (int i = 0; i < m->pending_count; i++) {
            if (!mo_is_release(m->pending[i])) { victim = i; break; }
        }
        if (victim < 0) { m->dropped++; return; }
        memmove(m->pending[victim], m->pending[victim + 1],
                (size_t)(m->pending_count - victim - 1) * 4);
        m->pending_count--;
        m->dropped++;
    }
    memcpy(m->pending[m->pending_count++], pkt, 4);
}

/* Send in order: anything already waiting goes first, and a packet the ring
 * refuses waits behind it rather than being lost. */
static void mo_emit(midi_out_t *m, uint8_t status, uint8_t d1, uint8_t d2) {
    if (!g_send_external) return;   /* non-shadow host: nowhere to send */
    const uint8_t pkt[4] = {
        (uint8_t)((MO_CABLE_EXTERNAL << 4) | (status >> 4)),  /* CIN = status type */
        status, d1, d2
    };
    mo_flush(m);
    if (m->pending_count == 0 && g_send_external(pkt, 4) > 0) return;
    mo_enqueue(m, pkt);
}

/* ---- Forwarding ---------------------------------------------------------- */

/* The output channel for a release of `note`: wherever it was last played
 * that has not been released yet, preferring the current channel. Falls back
 * to the current channel when nothing is tracked — a note-off for a note this
 * module never sent is harmless, and refusing to send it would be the one
 * choice that could leave something stuck. */
static int mo_release_channel(midi_out_t *m, uint8_t note) {
    if (m->refs[m->channel][note] > 0) return m->channel;
    for (int ch = 0; ch < 16; ch++) {
        if (m->refs[ch][note] > 0) return ch;
    }
    return m->channel;
}

static void mo_forward(midi_out_t *m, const uint8_t *msg, int len) {
    uint8_t type = msg[0] & 0xF0;
    if (type < 0x80 || type > 0xE0) return;              /* system: never sent */

    int two_byte = (type == 0xC0 || type == 0xD0);
    if (len < (two_byte ? 2 : 3)) return;
    uint8_t d1 = msg[1];
    uint8_t d2 = two_byte ? 0 : msg[2];
    if ((d1 & 0x80) || (d2 & 0x80)) return;              /* not MIDI */

    int ch = m->channel;

    if (type == 0x90 && d2 > 0) {
        if (m->refs[ch][d1] < 255) m->refs[ch][d1]++;
    } else if (type == 0x80 || type == 0x90) {
        ch = mo_release_channel(m, d1);
        if (m->refs[ch][d1] > 0) m->refs[ch][d1]--;
    } else if (type == 0xB0 && d1 == MO_CC_SUSTAIN) {
        if (d2 >= 64) {
            m->sustain_down |= (uint16_t)(1u << ch);
        } else {
            /* Lift the pedal everywhere we put it down, not just here. */
            uint16_t others = m->sustain_down & (uint16_t)~(1u << ch);
            m->sustain_down = 0;
            mo_emit(m, (uint8_t)(0xB0 | ch), d1, d2);
            for (int c = 0; c < 16; c++) {
                if (others & (1u << c)) mo_emit(m, (uint8_t)(0xB0 | c), MO_CC_SUSTAIN, 0);
            }
            return;
        }
    }

    mo_emit(m, (uint8_t)(type | ch), d1, d2);
}

/* Release everything this instance left sounding on the external synth. */
static void mo_release_all(midi_out_t *m) {
    for (int ch = 0; ch < 16; ch++) {
        for (int n = 0; n < 128; n++) {
            while (m->refs[ch][n] > 0) {
                m->refs[ch][n]--;
                mo_emit(m, (uint8_t)(0x80 | ch), (uint8_t)n, 0);
            }
        }
        if (m->sustain_down & (1u << ch)) {
            mo_emit(m, (uint8_t)(0xB0 | ch), MO_CC_SUSTAIN, 0);
        }
    }
    m->sustain_down = 0;
}

/* ---- Thru ---------------------------------------------------------------- */

/* Does this message continue down the chain? See the THRU note at the top. */
static int mo_thru_passes(midi_out_t *m, const uint8_t *msg, int len) {
    uint8_t status = msg[0];
    if (status < 0x80 || status >= 0xF0) return 1;      /* system: always */
    if (len < 3 || (msg[1] & 0x80)) return m->thru;      /* nothing to track */

    uint8_t type = status & 0xF0, ch = status & 0x0F;
    uint8_t d1 = msg[1], d2 = msg[2];
    uint16_t bit = (uint16_t)(1u << ch);
    int note_on  = (type == 0x90 && d2 > 0);
    int note_off = (type == 0x80 || (type == 0x90 && d2 == 0));
    int sustain  = (type == 0xB0 && d1 == MO_CC_SUSTAIN);
    int all_off  = (type == 0xB0 && (d1 == 120 || d1 == 123));

    if (m->thru) {
        if (note_on) {
            if (m->thru_refs[ch][d1] < 255) m->thru_refs[ch][d1]++;
        } else if (note_off) {
            if (m->thru_refs[ch][d1] > 0) m->thru_refs[ch][d1]--;
        } else if (sustain) {
            if (d2 >= 64) m->thru_sustain |= bit;
            else          m->thru_sustain &= (uint16_t)~bit;
        } else if (all_off) {
            memset(m->thru_refs[ch], 0, sizeof(m->thru_refs[ch]));
        }
        return 1;
    }

    /* Thru off: only what the slot's synth is owed. */
    if (note_off && m->thru_refs[ch][d1] > 0) {
        m->thru_refs[ch][d1]--;
        return 1;
    }
    if (sustain && d2 < 64 && (m->thru_sustain & bit)) {
        m->thru_sustain &= (uint16_t)~bit;
        return 1;
    }
    if (all_off) {
        int owed = 0;
        for (int n = 0; n < 128 && !owed; n++) owed = m->thru_refs[ch][n] > 0;
        if (owed) {
            memset(m->thru_refs[ch], 0, sizeof(m->thru_refs[ch]));
            return 1;
        }
    }
    return 0;
}

/* ---- Params -------------------------------------------------------------- */

/* Accepts "5", "5.0" or "5.000000" (a knob write). Returns 0-15, or -1 when the
 * value is not a number, so a stray write leaves the channel alone instead of
 * silently snapping it to 1. */
static int mo_parse_channel(const char *val) {
    if (!val) return -1;
    char *end = NULL;
    double d = strtod(val, &end);
    if (end == val) return -1;
    int c = (int)(d + 0.5);
    if (c < MO_CHANNEL_MIN) c = MO_CHANNEL_MIN;
    if (c > MO_CHANNEL_MAX) c = MO_CHANNEL_MAX;
    return c - 1;
}

/* "On"/"Off" (what get_param reports), the option index "1"/"0", or a
 * knob-style "1.000". Reads only the leading word, so it also parses a value
 * inside the state JSON. Anything else is -1: the setting is left alone. */
static int mo_parse_thru(const char *val) {
    if (!val) return -1;
    char w[8];
    size_t n = 0;
    while (n + 1 < sizeof(w) && (isalnum((unsigned char)val[n]) || val[n] == '.')) {
        w[n] = (char)tolower((unsigned char)val[n]);
        n++;
    }
    w[n] = '\0';
    if (strcmp(w, "on") == 0 || strcmp(w, "true") == 0) return 1;
    if (strcmp(w, "off") == 0 || strcmp(w, "false") == 0) return 0;
    char *end = NULL;
    double d = strtod(w, &end);
    if (n == 0 || end == w || *end) return -1;
    return d >= 0.5 ? 1 : 0;
}

/* Where a field's value starts in {"channel":N,"thru":"On"}, or NULL.
 * Hand-rolled like the other built-ins: there is no JSON library on this path
 * and nothing here may allocate. A field that is absent leaves its setting
 * alone, so a 0.1.0 state (channel only) restores with Thru at its default. */
static const char *mo_state_value(const char *json, const char *key) {
    if (!json) return NULL;
    char pat[16];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return NULL;
    const char *k = strstr(json, pat);
    if (!k) return NULL;
    const char *colon = strchr(k + n, ':');
    if (!colon) return NULL;
    colon++;
    while (*colon == ' ' || *colon == '\t' || *colon == '"') colon++;
    return colon;
}

/* ---- API ----------------------------------------------------------------- */

static void *mo_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    midi_out_t *m = (midi_out_t *)calloc(1, sizeof(midi_out_t));
    if (!m) return NULL;
    m->channel = 0;   /* channel 1 */
    m->thru = 1;      /* the 0.1.0 behaviour, and what a saved 0.1.0 slot expects */
    return m;
}

static void mo_destroy_instance(void *instance) {
    midi_out_t *m = (midi_out_t *)instance;
    if (!m) return;
    /* Removed from the chain with keys down: without this the external synth
     * holds them forever. Best effort — there is no later block to retry on. */
    mo_flush(m);
    mo_release_all(m);
    free(m);
}

static int mo_process_midi(void *instance,
                           const uint8_t *in_msg, int in_len,
                           uint8_t out_msgs[][3], int out_lens[],
                           int max_out) {
    midi_out_t *m = (midi_out_t *)instance;
    if (!m || !in_msg || in_len < 1) return 0;

    mo_forward(m, in_msg, in_len);

    /* What passes, passes unchanged, on its original channel: with Thru on the
     * rest of the slot behaves as if this module were not there. */
    if (!mo_thru_passes(m, in_msg, in_len)) return 0;
    if (max_out < 1 || in_len > 3) return 0;
    out_msgs[0][0] = in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int mo_tick(void *instance, int frames, int sample_rate,
                   uint8_t out_msgs[][3], int out_lens[], int max_out) {
    (void)frames; (void)sample_rate; (void)out_msgs; (void)out_lens; (void)max_out;
    midi_out_t *m = (midi_out_t *)instance;
    if (m) mo_flush(m);
    return 0;
}

static void mo_set_param(void *instance, const char *key, const char *val) {
    midi_out_t *m = (midi_out_t *)instance;
    if (!m || !key || !val) return;

    if (strcmp(key, "channel") == 0) {
        int c = mo_parse_channel(val);
        if (c >= 0) m->channel = c;
    } else if (strcmp(key, "thru") == 0) {
        int t = mo_parse_thru(val);
        if (t >= 0) m->thru = t;
    } else if (strcmp(key, "state") == 0) {
        int c = mo_parse_channel(mo_state_value(val, "channel"));
        int t = mo_parse_thru(mo_state_value(val, "thru"));
        if (c >= 0) m->channel = c;
        if (t >= 0) m->thru = t;
    }
}

static int mo_get_param(void *instance, const char *key, char *buf, int buf_len) {
    midi_out_t *m = (midi_out_t *)instance;
    if (!m || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "channel") == 0) {
        return snprintf(buf, buf_len, "%d", m->channel + 1);
    }
    if (strcmp(key, "thru") == 0) {
        return snprintf(buf, buf_len, "%s", m->thru ? "On" : "Off");
    }
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len, "{\"channel\":%d,\"thru\":\"%s\"}",
                        m->channel + 1, m->thru ? "On" : "Off");
    }
    if (strcmp(key, "dropped") == 0) {
        return snprintf(buf, buf_len, "%d", m->dropped);
    }
    if (strcmp(key, "chain_params") == 0) {
        int n = (int)strlen(CHAIN_PARAMS_JSON);
        if (n >= buf_len) return -1;
        memcpy(buf, CHAIN_PARAMS_JSON, (size_t)n + 1);
        return n;
    }
    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version      = MIDI_FX_API_VERSION,
    .create_instance  = mo_create_instance,
    .destroy_instance = mo_destroy_instance,
    .process_midi     = mo_process_midi,
    .tick             = mo_tick,
    .set_param        = mo_set_param,
    .get_param        = mo_get_param,
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_send_external = host ? host->midi_send_external : NULL;
    return &g_api;
}
