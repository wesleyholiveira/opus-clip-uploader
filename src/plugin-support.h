#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

extern void blog(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif

#ifndef BLOG
#define BLOG(...) blog(__VA_ARGS__)
#endif