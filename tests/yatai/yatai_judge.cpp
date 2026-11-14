// yatai_judge LOGDIR FILE.tja...: replays the input logs written by tools/judgesim.c
// through YataiDON's own JudgeCore, frame by frame the way Player::update does
// (begin_frame, the inputs that are due, end_frame), and prints the same trace.
#include <cstdio>
#include <fstream>

#include "libs/judge.h"
#include "libs/parsers/tja.h"

static const double FRAME_MS = 1000.0 / 60.0;

class Judge : public JudgeCore {
public:
    using JudgeCore::JudgeCore;
    int percent() const { return judge_gauge.has_value() ? judge_gauge->get_percent() : 0; }

protected:
    void on_judgment(const Note& note, Judgments judgment, std::optional<DrumType> hit_drum) override {
        printf("J %d %d %d\n", note.index, static_cast<int>(judgment), hit_drum ? static_cast<int>(*hit_drum) : 0);
    }
};

int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    for (int a = 2; a < argc; a++) {
        std::filesystem::path path(argv[a]);
        TJAParser parser(path, 0, 0);
        std::string name = path.filename().string();
        std::vector<int> diffs;
        for (const auto& [diff, cd] : parser.metadata.course_data) {
            if (diff < 5) {
                diffs.push_back(diff);
            }
        }
        for (int diff : diffs) {
            for (int profile = 0; profile < 6; profile++) {
                auto [notes, branch_m, branch_e, branch_n] = parser.notes_to_position(diff);
                prepare_chart(notes, branch_m, branch_e, branch_n, Modifiers{}, std::nullopt);
                Judge judge(diff, Modifiers{}, false);
                judge.load_chart(notes, branch_m, branch_e, branch_n, parser.metadata.course_data, std::nullopt);

                std::vector<std::pair<double, int>> log;
                std::ifstream in(std::string(argv[1]) + "/" + name + "." + std::to_string(diff) + "." +
                                 std::to_string(profile) + ".log");
                double ms;
                int drum;
                while (in >> ms >> drum) {
                    log.emplace_back(ms, drum);
                }
                printf("#run %s %d %d\n", name.c_str(), diff, profile);
                size_t cursor = 0;
                double end = judge.judge_end_ms() + 3000;
                for (int k = 0;; k++) {
                    double t = -2000.0 + k * FRAME_MS;
                    if (t > end) {
                        break;
                    }
                    judge.begin_frame(t);
                    while (cursor < log.size() && log[cursor].first <= t) {
                        judge.check_note(log[cursor].first, static_cast<DrumType>(log[cursor].second), t);
                        cursor++;
                    }
                    judge.end_frame(t);
                }
                printf("H");
                for (BranchDifficulty b : judge.branch_history) {
                    printf(" %d", static_cast<int>(b));
                }
                JudgeResult r = judge.result();
                printf("\nR score=%d good=%d ok=%d bad=%d combo=%d roll=%d clear=%d rainbow=%d gauge=%d\n", r.score,
                       r.good, r.ok, r.bad, r.max_combo, r.drumroll, r.cleared, r.rainbow, judge.percent());
            }
        }
    }
    return 0;
}
