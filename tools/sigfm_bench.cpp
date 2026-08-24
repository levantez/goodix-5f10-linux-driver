// Бенчмарк SIGFM на реальных кадрах сенсора (PGM 8-бит).
//   sigfm_bench a.pgm b.pgm c.pgm ...
// Печатает keypoints по каждому и матрицу попарных score. По именам файлов (префикс до '_'
// или до цифр) считает same-finger vs different-finger и предлагает порог.
#include "sigfm/sigfm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <regex>

struct Frame { std::string name, label; SigfmImgInfo* info; int kp; };

static unsigned char* load_pgm(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    char magic[3] = {0};
    int mx;
    if (fscanf(f, "%2s %d %d %d", magic, w, h, &mx) != 4 || strcmp(magic, "P5")) { fclose(f); return nullptr; }
    fgetc(f); // один разделитель после maxval
    int n = (*w) * (*h);
    unsigned char* buf = (unsigned char*)malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
    fclose(f);
    return buf;
}

// метка = имя файла без расширения и без завершающих цифр (self1.pgm,self2.pgm -> self)
static std::string label_of(const std::string& path) {
    std::string b = path.substr(path.find_last_of('/') + 1);
    b = b.substr(0, b.find_last_of('.'));
    size_t e = b.size();
    while (e > 0 && isdigit(b[e-1])) e--;
    return b.substr(0, e);
}

int main(int argc, char** argv) {
    std::vector<Frame> frames;
    for (int i = 1; i < argc; i++) {
        int w, h;
        unsigned char* pix = load_pgm(argv[i], &w, &h);
        if (!pix) { printf("не читается %s\n", argv[i]); continue; }
        SigfmImgInfo* info = sigfm_extract(pix, w, h);
        free(pix);
        int kp = info ? sigfm_keypoints_count(info) : -1;
        frames.push_back({argv[i], label_of(argv[i]), info, kp});
        printf("%-28s %dx%d  keypoints=%d\n", argv[i], w, h, kp);
    }
    printf("\nМатрица score (инлайеры):\n%-14s", "");
    for (auto& f : frames) printf("%-10s", f.label.c_str());
    printf("\n");
    std::vector<int> same, diff;
    for (size_t i = 0; i < frames.size(); i++) {
        printf("%-14s", frames[i].name.substr(frames[i].name.find_last_of('/')+1).c_str());
        for (size_t j = 0; j < frames.size(); j++) {
            int s = (frames[i].info && frames[j].info) ? sigfm_match_score(frames[i].info, frames[j].info) : -1;
            printf("%-10d", s);
            if (i < j) {
                if (frames[i].label == frames[j].label) same.push_back(s);
                else diff.push_back(s);
            }
        }
        printf("\n");
    }
    auto stat = [](std::vector<int>& v, const char* t){
        if (v.empty()) { printf("%s: нет пар\n", t); return; }
        int mn=v[0],mx=v[0]; double s=0; for(int x:v){mn=x<mn?x:mn;mx=x>mx?x:mx;s+=x;}
        printf("%s: n=%zu min=%d max=%d avg=%.1f\n", t, v.size(), mn, mx, s/v.size());
    };
    printf("\n");
    stat(same, "SAME-finger score");
    stat(diff, "DIFF-finger score");
    return 0;
}
