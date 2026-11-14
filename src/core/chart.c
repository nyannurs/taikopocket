/* Chart preparation: a port of prepare_chart(), compute_load_time(), the note
 * naming ("moji") and calculate_base_score() from YataiDON (GPL-3.0). */
#include "chart.h"

#include <math.h>
#include <stddef.h>

static double get_ms_per_measure(double bpm_val, double time_sig) {
    if (bpm_val == 0) {
        return 0;
    }
    return 60000 * (time_sig * 4) / bpm_val;
}

static void compute_load_time(tp_note *note, int scroll_type, const tp_chart *c) {
    const int note_half_w = TP_LANE_NOTE_HALF_W;
    const float travel_distance = TP_LANE_TRAVEL;
    bool horizontal = fabs(note->scroll_x) >= fabs(note->scroll_y);
    float base_pixels_per_ms =
        (float)(note->bpm / 240000 * (horizontal ? fabs(note->scroll_x) : fabs(note->scroll_y)) * travel_distance);
    if (base_pixels_per_ms == 0) {
        note->load_ms = note->hit_ms;
        note->unload_ms = note->hit_ms;
        return;
    }
    float normal_travel_ms = (travel_distance + note_half_w) / base_pixels_per_ms;

    if (!(note->flags & TP_NOTE_SUDDEN) || note->sudden_appear_ms == INFINITY) {
        if (scroll_type != TP_NMSCROLL) {
            double px_per_beat = (horizontal ? note->scroll_x : fabs(note->scroll_y)) * travel_distance / 4;
            double left = horizontal ? -(TP_LANE_JUDGE_X + note_half_w) : -(travel_distance + note_half_w);
            double right = travel_distance + note_half_w;
            double beat = tp_tempo_beat_at(c, note->hit_ms);
            double lo = beat - right / px_per_beat, hi = beat - left / px_per_beat;
            double first, last;
            tp_tempo_ms_within(c, fmin(lo, hi), fmax(lo, hi), &first, &last);
            note->load_ms = fmin(first, note->hit_ms);
            note->unload_ms = fmax(last, note->hit_ms);
            return;
        }
        note->load_ms = note->hit_ms - normal_travel_ms;
        note->unload_ms = note->hit_ms + normal_travel_ms;
        return;
    }
    note->load_ms = note->hit_ms - note->sudden_appear_ms;
    float movement_duration = (float)note->sudden_moving_ms;
    if (movement_duration <= 0) {
        movement_duration = normal_travel_ms;
    }
    float sudden_pixels_per_ms = travel_distance / movement_duration;
    float unload_offset = travel_distance / sudden_pixels_per_ms;
    note->unload_ms = note->hit_ms + unload_offset;
}

bool tp_note_unplayable(const tp_note *n, int scroll_type, const double *audio_end_ms) {
    return scroll_type != TP_NMSCROLL && audio_end_ms && n->type != TP_BARLINE && n->hit_ms > *audio_end_ms;
}

/* ---- note names under the lane ---- */

enum { IV_UNKNOWN = 0, IV_QUARTER = 1, IV_EIGHTH = 2, IV_TWELFTH = 3, IV_SIXTEENTH = 4, IV_TWENTYFOURTH = 6,
       IV_THIRTYSECOND = 8 };

static int get_note_interval_type(double interval_ms, double bpm) {
    const double time_sig = 4.0;
    if (bpm == 0) {
        return IV_UNKNOWN;
    }
    double ms_per_measure = get_ms_per_measure(bpm, time_sig) / time_sig;
    double tolerance = 15.0;
    if (fabs(interval_ms - ms_per_measure / 8.0) < tolerance) {
        return IV_EIGHTH;
    }
    if (fabs(interval_ms - ms_per_measure / 16.0) < tolerance) {
        return IV_SIXTEENTH;
    }
    if (fabs(interval_ms - ms_per_measure / 12.0) < tolerance) {
        return IV_TWELFTH;
    }
    if (fabs(interval_ms - ms_per_measure / 24.0) < tolerance) {
        return IV_TWENTYFOURTH;
    }
    if (fabs(interval_ms - ms_per_measure / 32.0) < tolerance) {
        return IV_THIRTYSECOND;
    }
    if (fabs(interval_ms - ms_per_measure / 4.0) < tolerance) {
        return IV_QUARTER;
    }
    return IV_UNKNOWN;
}

static bool is_roll_or_balloon(const tp_note *n) {
    return n->type == TP_ROLL_HEAD || n->type == TP_ROLL_HEAD_L || n->type == TP_BALLOON_HEAD ||
           n->type == TP_KUSUDAMA;
}

static void modifier_moji(tp_note *notes, int count) {
    static const uint8_t se_notes[10] = {0, 0, 3, 5, 6, 7, 8, 9, 10, 11};
    for (int i = 0; i < count; i++) {
        if (notes[i].type != TP_BARLINE) {
            notes[i].moji = se_notes[notes[i].type];
        }
    }
    static const int intervals[5] = {IV_EIGHTH, IV_TWELFTH, IV_SIXTEENTH, IV_TWENTYFOURTH, IV_THIRTYSECOND};
    for (int k = 0; k < 5; k++) {
        /* find_streams(): runs of notes spaced by this interval */
        int i = 0;
        while (count >= 2 && i < count - 1) {
            if (is_roll_or_balloon(&notes[i])) {
                i++;
                continue;
            }
            int start = i, length = 1;
            while (i < count - 1) {
                if (is_roll_or_balloon(&notes[i + 1])) {
                    break;
                }
                double interval = notes[i + 1].hit_ms - notes[i].hit_ms;
                if (get_note_interval_type(interval, notes[i].bpm) != intervals[k]) {
                    break;
                }
                length++;
                i++;
            }
            if (length >= 2) {
                for (int j = start; j < start + length - 1; j++) {
                    if (notes[j].type == TP_DON) {
                        notes[j].moji = 1;
                    } else if (notes[j].type == TP_KAT) {
                        notes[j].moji = 4;
                    }
                }
                if (length == 3 && notes[start].type == TP_DON && notes[start + 1].type == TP_DON &&
                    notes[start + 2].type == TP_DON) {
                    notes[start + 1].moji = 2;
                }
            }
            i++;
        }
    }
}

/* YataiDON modifier_random: of the notes of a list (everything but bar lines), a fifth
 * or two fifths are picked, and those of them that are don or ka are swapped. YataiDON
 * shuffles the indices with a Mersenne Twister and takes the first ones; here every
 * note is taken with the chance (still to pick) / (still to come), which picks a set
 * of the same size with the same chances and needs no list of indices. Every list of a
 * chart starts from the same seed, as in YataiDON. */
static void modifier_random(tp_note *notes, int count, int value, uint32_t seed, const uint8_t *swap) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += notes[i].type != TP_BARLINE;
    }
    int pick = total / 5 * value;
    pick = pick > total ? total : pick;
    uint32_t state = seed * 2654435761u + 0x9E3779B9u;
    state = state ? state : 1;
    for (int i = 0; i < count && pick > 0; i++) {
        if (notes[i].type == TP_BARLINE) {
            continue;
        }
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        if ((state >> 8) % (uint32_t)total < (uint32_t)pick) {
            notes[i].type = swap[notes[i].type];
            pick--;
        }
        total--;
    }
}

static void apply_modifiers(tp_note *notes, int count, const tp_modifiers *mods) {
    static const uint8_t inverse[10] = {0, TP_KAT, TP_DON, TP_KAT_L, TP_DON_L, 5, 6, 7, 8, 9};
    if (mods->invisible) { /* YataiDON modifier_display */
        for (int i = 0; i < count; i++) {
            if (notes[i].type != TP_BARLINE) {
                notes[i].flags &= (uint8_t)~TP_NOTE_DISPLAY;
            }
        }
    }
    if (mods->inverse) {
        for (int i = 0; i < count; i++) {
            notes[i].type = inverse[notes[i].type];
        }
    }
    if (mods->random) {
        modifier_random(notes, count, mods->random, mods->seed, inverse);
    }
    for (int i = 0; i < count; i++) {
        if (notes[i].type == TP_KUSUDAMA && mods->no_kusudama) {
            notes[i].type = TP_BALLOON_HEAD;
        }
        notes[i].scroll_x *= mods->speed;
    }
    modifier_moji(notes, count);
}

void tp_chart_prepare(tp_chart *c, const tp_modifiers *mods, const double *audio_end_ms) {
    apply_modifiers(c->master, c->master_count, mods);
    tp_note *last_note = NULL;
    for (int i = 0; i < c->master_count; i++) {
        tp_note *note = &c->master[i];
        compute_load_time(note, c->scroll_type, c);
        if (note->type == TP_TAIL && last_note != NULL) {
            note->load_ms = last_note->load_ms;
            last_note->unload_ms = note->unload_ms;
        }
        if (note->type != TP_BARLINE) {
            last_note = note;
        }
    }
    for (int s = 0; s < c->section_count; s++) {
        tp_section *sec = &c->sections[s];
        tp_note *notes = c->branch + sec->first_note;
        apply_modifiers(notes, sec->note_count, mods);
        int kept = 0;
        for (int i = 0; i < sec->note_count; i++) {
            if (!tp_note_unplayable(&notes[i], c->scroll_type, audio_end_ms)) {
                notes[kept++] = notes[i];
            }
        }
        sec->note_count = kept;
        tp_note *section_last = NULL;
        for (int i = 0; i < sec->note_count; i++) {
            tp_note *note = &notes[i];
            compute_load_time(note, c->scroll_type, c);
            if (note->type == TP_TAIL && section_last != NULL) {
                note->load_ms = section_last->load_ms;
                section_last->unload_ms = note->unload_ms;
            }
            section_last = note; /* bar lines included, as in YataiDON */
        }
    }
}

int tp_chart_base_score(const tp_chart *c, const double *audio_end_ms) {
    int total_notes = 0;
    int balloon_count = 0;
    double drumroll_msec = 0.0;

    /* the list YataiDON scores: playable master notes, then the master-branch sections.
     * A drumroll lasts until the next element of that list, whatever it is. */
    const tp_note *prev = NULL;
    for (int l = -1; l < c->section_count; l++) {
        const tp_note *notes = c->master;
        int count = c->master_count;
        if (l >= 0) {
            if (c->sections[l].path != 2) {
                continue;
            }
            notes = c->branch + c->sections[l].first_note;
            count = c->sections[l].note_count;
        }
        for (int i = 0; i < count; i++) {
            const tp_note *note = &notes[i];
            if (l < 0 && tp_note_unplayable(note, c->scroll_type, audio_end_ms)) {
                continue;
            }
            if (prev && (prev->type == TP_ROLL_HEAD || prev->type == TP_ROLL_HEAD_L)) {
                drumroll_msec += note->hit_ms - prev->hit_ms;
            }
            prev = note;
            if (note->type == TP_ROLL_HEAD || note->type == TP_ROLL_HEAD_L) {
                continue;
            }
            if (note->count != TP_NO_COUNT) {
                balloon_count += note->count < 100 ? note->count : 100;
            } else if (note->type >= TP_DON && note->type <= TP_KAT_L) {
                total_notes += 1;
            }
        }
    }
    if (total_notes == 0) {
        return 1000000;
    }
    double calculation =
        (1000000.0f - (balloon_count * 100.0f) - (16.920079999994086f * drumroll_msec / 1000.0f * 100.0f)) /
        total_notes / 10.0f;
    return (int)ceil(calculation) * 10;
}
