#pragma once

#ifndef ZXLOG_BUILD_MODE
#define ZXLOG_BUILD_MODE 3
#endif
#if ZXLOG_BUILD_MODE < 1 || ZXLOG_BUILD_MODE > 3
#error "ZXLOG_BUILD_MODE must be 1=TEXT, 2=BINARY, or 3=RUNTIME"
#endif
