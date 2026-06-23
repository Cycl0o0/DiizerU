// Single translation unit that compiles the minimp3 implementation. Everywhere
// else includes minimp3.h for declarations only.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "../../third_party/minimp3/minimp3.h"
