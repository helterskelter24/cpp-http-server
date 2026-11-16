#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#include "app/static_files.h"
#include "http/mime.h"
#include "http/router.h"
#include "test.h"

namespace {

// Creates a fresh directory under /tmp and removes everything it contains
// (files, symlinks, subdirectories) when it goes out of scope.
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/hs_static_XXXXXX";
        const char* made = mkdtemp(tmpl);
        if (made == nullptr) {
            // Carrying on with an empty path would make every write() below
            // land in "/" + rel, the filesystem root. Nothing in this file can
            // run without the directory, so stop the whole test binary.
            std::perror("mkdtemp /tmp/hs_static_XXXXXX");
            std::abort();
        }
        path_ = made;
    }
    ~TempDir() {
        // Recursive delete of a directory this test created itself; the path
        // comes from mkdtemp and contains no quote characters.
        std::string cmd = "rm -rf '" + path_ + "'";
        if (system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup of %s failed\n", path_.c_str());
    }
    const std::string& path() const { return path_; }
    void write(const std::string& rel, const std::string& content) const {
        std::ofstream out(path_ + "/" + rel, std::ios::binary);
        out << content;
    }
    void mkdir(const std::string& rel) const { ::mkdir((path_ + "/" + rel).c_str(), 0755); }

private:
    std::string path_;
};

hs::Request get(const std::string& path) {
    hs::Request req;
    req.method = hs::Method::GET;
    req.path = path;
    req.target = path;
    return req;
}

const std::string* header(const hs::Response& r, const std::string& name) {
    for (const auto& h : r.headers)
        if (h.first == name) return &h.second;
    return nullptr;
}

// Layout: <base>/secret (outside) and <base>/www (the served root).
struct Site {
    TempDir base;
    std::string root;
    Site() {
        base.write("secret", "top secret");
        base.mkdir("www");
        root = base.path() + "/www";
    }
};

}  // namespace

TEST(mime_table) {
    CHECK_EQ(hs::mime_type("/a/index.html"), "text/html");
    CHECK_EQ(hs::mime_type("x.css"), "text/css");
    CHECK_EQ(hs::mime_type("x.js"), "application/javascript");
    CHECK_EQ(hs::mime_type("x.json"), "application/json");
    CHECK_EQ(hs::mime_type("x.txt"), "text/plain");
    CHECK_EQ(hs::mime_type("x.png"), "image/png");
    CHECK_EQ(hs::mime_type("x.jpg"), "image/jpeg");
    CHECK_EQ(hs::mime_type("x.jpeg"), "image/jpeg");
    CHECK_EQ(hs::mime_type("x.gif"), "image/gif");
    CHECK_EQ(hs::mime_type("x.svg"), "image/svg+xml");
    CHECK_EQ(hs::mime_type("x.ico"), "image/x-icon");
    CHECK_EQ(hs::mime_type("x.pdf"), "application/pdf");
    CHECK_EQ(hs::mime_type("x.wasm"), "application/wasm");
    CHECK_EQ(hs::mime_type("X.HTML"), "text/html");
    // Bytes >= 0x80 are not letters to fold: no match, and no undefined behaviour.
    CHECK_EQ(hs::mime_type("x.\xc3\x89"), "application/octet-stream");
    CHECK_EQ(hs::mime_type("noext"), "application/octet-stream");
    CHECK_EQ(hs::mime_type("x.unknown"), "application/octet-stream");
    // A dot in a directory name is not an extension.
    CHECK_EQ(hs::mime_type("/v1.2/readme"), "application/octet-stream");
    CHECK_EQ(hs::mime_type(""), "application/octet-stream");
}

TEST(static_serves_file_with_mime) {
    Site s;
    s.base.write("www/a.css", "body{}");
    s.base.write("www/b.png", "PNGDATA");
    hs::StaticFiles files(s.root);
    CHECK(files.root_ok());

    hs::Response r = files.serve(get("/a.css"));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "body{}");
    const std::string* ct = header(r, "Content-Type");
    CHECK(ct != nullptr);
    if (ct) CHECK_EQ(*ct, "text/css; charset=utf-8");

    hs::Response p = files.serve(get("/b.png"));
    CHECK_EQ(p.status, 200);
    const std::string* pct = header(p, "Content-Type");
    CHECK(pct != nullptr);
    if (pct) CHECK_EQ(*pct, "image/png");  // binary types get no charset
}

TEST(static_index_for_directory) {
    Site s;
    s.base.write("www/index.html", "<h1>hi</h1>");
    s.base.mkdir("www/sub");
    s.base.write("www/sub/index.html", "sub");
    hs::StaticFiles files(s.root);
    hs::Response root = files.serve(get("/"));
    CHECK_EQ(root.status, 200);
    CHECK_EQ(root.body, "<h1>hi</h1>");
    hs::Response sub = files.serve(get("/sub"));
    CHECK_EQ(sub.status, 200);
    CHECK_EQ(sub.body, "sub");
    hs::Response sub2 = files.serve(get("/sub/"));
    CHECK_EQ(sub2.status, 200);
    // Repeated slashes collapse in realpath.
    hs::Response slashes = files.serve(get("//sub//index.html"));
    CHECK_EQ(slashes.status, 200);
}

TEST(static_missing_404) {
    Site s;
    hs::StaticFiles files(s.root);
    CHECK_EQ(files.serve(get("/nope.txt")).status, 404);
}

TEST(static_directory_without_index_404) {
    Site s;
    s.base.mkdir("www/empty");
    hs::StaticFiles files(s.root);
    CHECK_EQ(files.serve(get("/empty")).status, 404);
    CHECK_EQ(files.serve(get("/")).status, 404);
}

TEST(static_dotdot_blocked) {
    Site s;
    s.base.write("www/ok.txt", "ok");
    s.base.mkdir("www/a");
    hs::StaticFiles files(s.root);
    CHECK_EQ(files.serve(get("/../secret")).status, 404);
    CHECK_EQ(files.serve(get("/a/../../secret")).status, 404);
    CHECK_EQ(files.serve(get("/../www/../secret")).status, 404);
    // ".." that stays inside the root is harmless and resolves normally.
    CHECK_EQ(files.serve(get("/a/../ok.txt")).status, 200);
}

TEST(static_sibling_prefix_blocked) {
    // A sibling directory whose name starts with the root's name ("www2") must
    // not pass a plain string-prefix check.
    Site s;
    s.base.mkdir("www2");
    s.base.write("www2/x.txt", "sibling");
    hs::StaticFiles files(s.root);
    CHECK_EQ(files.serve(get("/../www2/x.txt")).status, 404);
}

TEST(static_symlink_escape_blocked) {
    Site s;
    CHECK_EQ(symlink((s.base.path() + "/secret").c_str(), (s.root + "/link").c_str()), 0);
    CHECK_EQ(symlink(s.base.path().c_str(), (s.root + "/dirlink").c_str()), 0);
    hs::StaticFiles files(s.root);
    CHECK_EQ(files.serve(get("/link")).status, 404);
    CHECK_EQ(files.serve(get("/dirlink/secret")).status, 404);
}

TEST(static_symlink_inside_allowed) {
    Site s;
    s.base.write("www/real.txt", "real");
    CHECK_EQ(symlink("real.txt", (s.root + "/alias.txt").c_str()), 0);
    hs::StaticFiles files(s.root);
    hs::Response r = files.serve(get("/alias.txt"));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "real");
}

TEST(static_odd_paths_are_safe) {
    Site s;
    hs::StaticFiles files(s.root);
    CHECK_EQ(files.serve(get(std::string("/a\0b", 4))).status, 404);
    CHECK_EQ(files.serve(get("/a\nb")).status, 404);
    CHECK_EQ(files.serve(get("")).status, 404);  // no leading slash; root has no index.html
    CHECK_EQ(files.serve(get(std::string(5000, 'a'))).status, 404);  // exceeds PATH_MAX
}

TEST(static_non_regular_file_404) {
    Site s;
    // A FIFO resolves inside the root but is not a regular file; opening it
    // must not block the worker thread.
    CHECK_EQ(mkfifo((s.root + "/pipe").c_str(), 0600), 0);
    hs::StaticFiles files(s.root);
    CHECK_EQ(files.serve(get("/pipe")).status, 404);
}

TEST(static_unreadable_403) {
    if (geteuid() == 0) return;  // root ignores file permissions
    Site s;
    s.base.write("www/locked.txt", "x");
    chmod((s.root + "/locked.txt").c_str(), 0);
    hs::StaticFiles files(s.root);
    CHECK_EQ(files.serve(get("/locked.txt")).status, 403);
}

TEST(static_bad_root) {
    // The constructor logs an error on purpose; keep the test output clean.
    int saved = dup(STDERR_FILENO);
    int null_fd = open("/dev/null", O_WRONLY);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
    hs::StaticFiles files("/nonexistent/hs/root");
    dup2(saved, STDERR_FILENO);
    close(saved);
    CHECK(!files.root_ok());
    CHECK_EQ(files.serve(get("/x")).status, 404);
}

TEST(static_binary_and_large_file_round_trip) {
    Site s;
    std::string data;
    for (int i = 0; i < 300000; ++i) data.push_back(static_cast<char>(i * 31 + 7));
    s.base.write("www/data.bin", data);
    hs::StaticFiles files(s.root);
    hs::Response r = files.serve(get("/data.bin"));
    CHECK_EQ(r.status, 200);
    CHECK(r.body == data);
    const std::string* ct = header(r, "Content-Type");
    CHECK(ct != nullptr);
    if (ct) CHECK_EQ(*ct, "application/octet-stream");
}

TEST(static_fallback_registration) {
    Site s;
    s.base.write("www/f.txt", "via router");
    hs::Router router;
    hs::register_static(router, std::make_shared<hs::StaticFiles>(s.root));
    hs::Request req = get("/f.txt");
    hs::Response r = router.dispatch(req);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "via router");
}

TEST(static_oversize_file_500) {
    Site s;
    // A sparse file: ftruncate sets the size without writing data, and serve()
    // checks the size before reading, so this costs no disk space or time.
    std::string path = s.root + "/huge.bin";
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK_EQ(ftruncate(fd, static_cast<off_t>(hs::kMaxStaticFileBytes + 1)), 0);
    close(fd);
    hs::StaticFiles files(s.root);
    // The refusal is logged on purpose; keep the test output clean.
    int saved = dup(STDERR_FILENO);
    int null_fd = open("/dev/null", O_WRONLY);
    dup2(null_fd, STDERR_FILENO);
    close(null_fd);
    hs::Response r = files.serve(get("/huge.bin"));
    dup2(saved, STDERR_FILENO);
    close(saved);
    CHECK_EQ(r.status, 500);
}
