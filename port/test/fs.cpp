// VFS smoke test: pack reads, directory walks, the /user RAM filesystem.
#include <cstdio>
#include <filesystem>
#include <dirent.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static void walk(const std::string &dir, int depth)
{
    DIR *d = opendir(dir.c_str());
    if (!d) { std::printf("%*sopendir(%s) failed\n", depth * 2, "", dir.c_str()); return; }
    while (dirent *e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st;
        int rc = stat(path.c_str(), &st);
        std::printf("%*s%s type=%d stat=%d dir=%d size=%ld\n", depth * 2, "", name.c_str(), e->d_type, rc, S_ISDIR(st.st_mode), (long)st.st_size);
        if (S_ISDIR(st.st_mode) && depth < 3) walk(path, depth + 1);
    }
    closedir(d);
}

int main()
{
    char cwd[200];
    std::printf("cwd=%s\n", getcwd(cwd, sizeof(cwd)));
    walk("data/gfx", 0);
    std::ifstream in("data/gfx/radium/tileset.txt");
    std::string line;
    while (std::getline(in, line)) std::printf("tileset: %s\n", line.c_str());
    std::ofstream out("/user/test/a.txt");
    std::printf("user write open=%d\n", (int)out.is_open());
    mkdir("/user/test", 0755);
    std::ofstream out2("/user/test/a.txt");
    out2 << "hello user\n";
    out2.close();
    std::ifstream back("/user/test/a.txt");
    std::getline(back, line);
    std::printf("user read: %s\n", line.c_str());
    namespace fs = std::filesystem;
    try {
        std::printf("absolute: %s\n", fs::absolute("data/gfx").c_str());
        std::printf("weakly_canonical: %s\n", fs::weakly_canonical("data/gfx/../gfx").c_str());
        std::printf("exists: %d is_dir: %d\n", (int)fs::exists("data/gfx"), (int)fs::is_directory("data/gfx"));
        for (const auto &e : fs::directory_iterator("data/gfx")) std::printf("entry: %s dir=%d\n", e.path().c_str(), (int)e.is_directory());
        for (const auto &e : fs::recursive_directory_iterator("data/json/items/armor")) { std::printf("rec: %s\n", e.path().c_str()); break; }
    } catch (const std::exception &ex) {
        std::printf("fs exception: %s\n", ex.what());
    }
    return 0;
}
