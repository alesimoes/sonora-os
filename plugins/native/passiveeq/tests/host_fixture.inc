    // Iron is a reconfigure parameter with no latency change. The host
    // fixture refuses a reconfiguring parameter outside the reactivate case,
    // so the plain cases take the golden config without it, and the golden
    // file (rendered with iron on) is not compared here; the golden test
    // keeps it on. The reconfigure case sets it back.
    {"passiveeq", [] { return native::make_passiveeq(); },
     [] {
         Config c = native::suites::passiveeq_golden_config();
         for (auto it = c.begin(); it != c.end();)
             it = std::strcmp(it->first, "iron") == 0 ? c.erase(it) : it + 1;
         return c;
     },
     true, {"lf_boost", 6.0f}, {{"iron", 1.0f}}, false},
