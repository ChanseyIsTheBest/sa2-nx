// rr6-nx tools/test_lz4_blocks.cpp: the SDK's log macros as plain prints of the format text.
#pragma once
#include <cstdio>
#define REXLOG_ERROR(fmt_text, ...) std::fprintf(stderr, "[lz4b error] %s\n", fmt_text)
#define REXLOG_INFO(fmt_text, ...) std::fprintf(stderr, "[lz4b info] %s\n", fmt_text)
