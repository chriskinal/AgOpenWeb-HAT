#include "aow/log.h"

#include <cstring>
#include <string>
#include <vector>

#include "check.h"

using namespace aow::log;

int main() {
    uint32_t now = 1000;
    Logger lg([&] { return now; });

    std::vector<std::string> console, udp, web;
    Logger::Outputs out;
    out.console = [&](const Entry& e) { console.push_back(e.message); };
    out.udp = [&](const Entry& e) { udp.push_back(e.message); };
    out.web = [&](const Entry& e) { web.push_back(e.message); };

    // Startup: every level reaches the console, and nothing is rate-limited.
    for (int i = 0; i < 30; ++i) lg.log(Severity::Debug, Source::Gnss, "boot %d", i);
    CHECK(lg.drain(out) == 30);
    CHECK(console.size() == 30 && web.size() == 30 && udp.empty());  // UDP off by default
    CHECK(console[7] == "boot 7");

    // After startup the console level applies (INFO by default)...
    lg.setStartupDone();  // logs one INFO line itself
    console.clear(); web.clear();
    lg.log(Severity::Debug, Source::Can, "hidden");
    lg.log(Severity::Warning, Source::Can, "shown %s", "here");
    lg.drain(out);
    CHECK(console.size() == 2 && console[0].rfind("System initialization complete", 0) == 0);
    CHECK(console.size() == 2 && console[1] == "shown here");
    CHECK(web.size() == 3);  // the web viewer gets every level

    // ...and the rate limit too: DEBUG allows 5 a second, a full bucket to start.
    now += 2000;
    for (int i = 0; i < 20; ++i) lg.log(Severity::Debug, Source::Can, "burst");
    CHECK(lg.droppedByRateLimit() == 15);
    now += 200;  // 0.2 s refills one DEBUG token
    lg.log(Severity::Debug, Source::Can, "one more");
    lg.log(Severity::Debug, Source::Can, "too soon");
    CHECK(lg.droppedByRateLimit() == 16);

    // Rate limit off: nothing dropped.
    Config c = lg.config();
    c.rateLimitDisabled = true;
    c.udpEnabled = true;
    c.udpLevel = Severity::Error;
    lg.setConfig(c);
    for (int i = 0; i < 80; ++i) lg.log(Severity::Debug, Source::Can, "flood");
    CHECK(lg.droppedByRateLimit() == 16);

    // UDP gets only ERROR and worse.
    lg.drain(out);
    udp.clear();
    lg.log(Severity::Warning, Source::System, "warn");
    lg.log(Severity::Critical, Source::System, "crit");
    lg.drain(out);
    CHECK(udp.size() == 1 && udp[0] == "crit");

    // History holds the newest 100, oldest first, with rising sequence numbers.
    Entry h[Logger::kHistory];
    const size_t n = lg.history(h, Logger::kHistory);
    CHECK(n == Logger::kHistory);
    CHECK(std::strcmp(h[n - 1].message, "crit") == 0);
    CHECK(h[n - 1].sequence == h[n - 2].sequence + 1);
    CHECK(h[n - 1].severity == Severity::Critical && h[n - 1].source == Source::System);
    Entry last2[2];
    CHECK(lg.history(last2, 2) == 2 && std::strcmp(last2[1].message, "crit") == 0);

    // Long messages are cut, not overrun.
    lg.log(Severity::Error, Source::User, "%s", std::string(500, 'x').c_str());
    lg.history(last2, 2);
    CHECK(std::strlen(last2[1].message) == sizeof last2[1].message - 1);

    // No drain for a while: the queue keeps the newest kPending, counts the rest.
    lg.drain(out);
    for (size_t i = 0; i < Logger::kPending + 10; ++i) lg.log(Severity::Error, Source::User, "q%zu", i);
    CHECK(lg.droppedPending() == 10);
    web.clear();
    CHECK(lg.drain(out) == Logger::kPending);
    CHECK(web.front() == "q10");

    // What a new log viewer is sent: the history without the entries still waiting for drain().
    lg.log(Severity::Error, Source::User, "waiting1");
    lg.log(Severity::Error, Source::User, "waiting2");
    CHECK(lg.history(last2, 2) == 2 && std::strcmp(last2[1].message, "waiting2") == 0);
    CHECK(lg.drainedHistory(last2, 2) == 2 && std::strcmp(last2[1].message, web.back().c_str()) == 0);
    CHECK(lg.drainedHistory(h, Logger::kHistory) == Logger::kHistory - 2);
    lg.drain(out);
    CHECK(lg.drainedHistory(last2, 2) == 2 && std::strcmp(last2[1].message, "waiting2") == 0);

    // Severity names, both forms.
    Severity s;
    CHECK(parseSeverity("warn", s) && s == Severity::Warning);
    CHECK(parseSeverity("WARNING", s) && s == Severity::Warning);
    CHECK(!parseSeverity("loud", s));
    CHECK(std::strcmp(sourceName(Source::Autosteer), "STEER") == 0);

    return finish("log");
}
