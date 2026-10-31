#pragma once
// Runs tools/fetch_real_catalogs.py in the background (the real galaxy catalogue may not be
// redistributed, so it is downloaded on the user's machine), reporting progress for the panel.
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Log.h"

namespace bhs {

class CatalogFetch {
public:
    enum State { Idle = 0, Running = 1, Done = 2, Failed = 3 };

    // Starts the download unless one is running. appRoot: directory holding tools/ and assets/.
    void start(const std::string& appRoot) {
        if (shared_->state.load() == Running) return;
        shared_->state = Running;
        {
            std::lock_guard<std::mutex> lock(shared_->m);
            shared_->line = "starting...";
        }
        const std::string cmd = "python3 -I \"" + appRoot + "/tools/fetch_real_catalogs.py\" 2>&1";
        // The thread owns a reference to the shared state, so it may outlive the app (it is detached).
        std::thread([s = shared_, cmd] {
            FILE* f = popen(cmd.c_str(), "r");
            if (!f) {
                std::lock_guard<std::mutex> lock(s->m);
                s->line = "could not start python3";
                s->state = Failed;
                return;
            }
            char buf[512];
            while (std::fgets(buf, sizeof buf, f)) {
                std::string l(buf);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                if (l.empty()) continue;
                logInfo("catalogue download: {}", l);   // the terminal gets every line, the panel the last one
                std::lock_guard<std::mutex> lock(s->m);
                // A long line is cut from the left: the script's failure lines end with the reason.
                s->line = l.size() > 120 ? "..." + l.substr(l.size() - 117) : l;
            }
            const int rc = pclose(f);
            if (rc != 0) logWarn("galaxy catalogue download failed (see the lines above)");
            s->state = rc == 0 ? Done : Failed;
        }).detach();
    }

    State state() const { return static_cast<State>(shared_->state.load()); }
    std::string lastLine() const {
        std::lock_guard<std::mutex> lock(shared_->m);
        return shared_->line;
    }
    // True once after a successful download (the caller reloads the universe).
    bool takeFinished() {
        int expected = Done;
        return shared_->state.compare_exchange_strong(expected, Idle);
    }

private:
    struct Shared {
        std::atomic<int> state{Idle};
        mutable std::mutex m;
        std::string line;
    };
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};

}  // namespace bhs
