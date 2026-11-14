// yatai_dump FILE.tja...: the reference side of the golden test. Links YataiDON's
// own parser (third_party/YataiDON) and prints every course in the format of
// tools/tjadump.c. Charts must be UTF-8.
#include <cstdio>
#include <sstream>

#include "libs/judge.h"
#include "libs/parsers/tja.h"

static void dump_notes(const char* list, const std::deque<Note>& notes) {
    int i = 0;
    for (const Note& n : notes) {
        printf("N %s %d t=%d i=%d hit=%.17g bpm=%.17g sx=%.17g sy=%.17g d=%d bs=%d br=%d p=%d", list, i++,
               static_cast<int>(n.type), n.index, n.hit_ms, n.bpm, n.scroll_x, n.scroll_y, n.display, n.bar_start,
               n.is_branch_start, n.partner);
        if (n.count.has_value()) {
            printf(" c=%d", n.count.value());
        }
        if (n.sudden_appear_ms.has_value()) {
            printf(" sa=%.17g sm=%.17g", n.sudden_appear_ms.value(), n.sudden_moving_ms.value());
        }
        printf(" load=%.17g unload=%.17g m=%d\n", n.load_ms, n.unload_ms, n.moji);
    }
}

static void dump_events(const char* list, const std::deque<TimelineObject>& timeline) {
    for (const TimelineObject& e : timeline) {
        if (e.bpm.has_value()) {
            printf("E %s bpm %.17g %.17g\n", list, e.start_time, e.bpm.value());
        } else if (e.gogo_time.has_value()) {
            printf("E %s gogo %.17g %d\n", list, e.start_time, e.gogo_time.value());
        } else if (e.branch_params.has_value()) {
            // what JudgeCore::handle_judge_entry makes of the parameters
            std::vector<std::string> parts;
            std::stringstream ss(e.branch_params.value());
            std::string part;
            while (std::getline(ss, part, ',')) {
                parts.push_back(part);
            }
            float e_req = 0, m_req = 0;
            bool ok = parts.size() >= 3;
            char cond = '-';
            if (ok) {
                cond = parts[0] == "p" ? 'p' : parts[0] == "r" ? 'r' : '-';
                try {
                    e_req = std::stof(parts[1]);
                    m_req = std::stof(parts[2]);
                } catch (const std::exception&) {
                    e_req = m_req = 0;
                }
            }
            printf("E %s branch %.17g ok=%d cond=%c e=%.9g m=%.9g\n", list, e.start_time, ok, cond, e_req, m_req);
        } else if (e.section_reset.has_value()) {
            printf("E %s section %.17g\n", list, e.start_time);
        } else if (e.delta_x.has_value()) {
            printf("E %s jpos %.17g %.17g jx=%.17g jy=%.17g dx=%.17g dy=%.17g\n", list, e.start_time, e.end_time,
                   e.judge_pos_x.value(), e.judge_pos_y.value(), e.delta_x.value(), e.delta_y.value());
        } else if (e.lyric.has_value()) {
            printf("E %s lyric %.17g [%s]\n", list, e.start_time, e.lyric.value().c_str());
        }
    }
}

static void dump_ints(const char* name, const std::vector<int>& v) {
    printf(" %s=[", name);
    for (size_t i = 0; i < v.size(); i++) {
        printf("%s%d", i ? "," : "", v[i]);
    }
    printf("]");
}

int main(int argc, char** argv) {
    for (int a = 1; a < argc; a++) {
        std::filesystem::path path(argv[a]);
        TJAParser parser(path, 0, 0);
        printf("#file %s\n", path.filename().string().c_str());
        printf("meta bpm=%.17g offset=%.17g demostart=%.17g title=[%s] subtitle=[%s] full=%d\n", parser.metadata.bpm,
               parser.metadata.offset, parser.metadata.demostart, parser.metadata.title["en"].c_str(),
               parser.metadata.subtitle["en"].c_str(), parser.metadata.subtitle_full_display);
        std::vector<int> diffs;
        for (const auto& [diff, cd] : parser.metadata.course_data) {
            diffs.push_back(diff);
        }
        for (int diff : diffs) {
            auto [notes, branch_m, branch_e, branch_n] = parser.notes_to_position(diff);
            prepare_chart(notes, branch_m, branch_e, branch_n, Modifiers{}, std::nullopt);
            NoteList total;
            total.notes = notes.notes;
            for (const NoteList& section : branch_m) {
                total.notes.insert(total.notes.end(), section.notes.begin(), section.notes.end());
            }
            const CourseData& cd = parser.metadata.course_data[diff];
            printf("== course %d level=%.17g branching=%d scroll=%d base=%d", diff, cd.level, cd.is_branching,
                   static_cast<int>(notes.scroll_type), calculate_base_score(total));
            dump_ints("nor", cd.branch_balloon[0]);
            dump_ints("exp", cd.branch_balloon[1]);
            dump_ints("mas", cd.branch_balloon[2]);
            printf("\n");
            dump_notes("master", notes.notes);
            dump_events("master", notes.timeline);
            for (const TempoPoint& p : notes.tempo_map.points) {
                printf("P %.17g %.17g %.17g\n", p.ms, p.beat, p.bpm);
            }
            const std::deque<NoteList>* paths[3] = {&branch_n, &branch_e, &branch_m};
            for (int p = 0; p < 3; p++) {
                int ordinal = 0;
                for (const NoteList& section : *paths[p]) {
                    char list[16];
                    snprintf(list, sizeof list, "%c%d", "NEM"[p], ordinal++);
                    dump_notes(list, section.notes);
                    dump_events(list, section.timeline);
                }
            }
        }
    }
    return 0;
}
