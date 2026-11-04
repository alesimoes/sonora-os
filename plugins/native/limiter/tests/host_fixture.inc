    {"limiter", [] { return native::make_limiter(); }, native::suites::limiter_golden_config, true,
     {"ceiling", -4.0f}, {{"lookahead", 3.0f}}},
