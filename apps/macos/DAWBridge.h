#ifndef MY_DAW_MACOS_BRIDGE_H
#define MY_DAW_MACOS_BRIDGE_H
#include "../../engine/bridge/daw.h"
#include <string.h>

/* Swift's Clang importer cannot expose the 4097-element C array as a tuple.
 * Keep its layout in C and copy into caller-owned storage. Header-only AppKit
 * adapter: no exported ABI, borrowed pointer or guessed byte offset. */
enum { DAW_MACOS_VST3_PATH_CAPACITY = sizeof(((daw_vst3_component*)0)->module_path) };
static inline int daw_macos_copy_vst3_path(const daw_vst3_component* source,
                                          char* output, size_t capacity) {
    if (output && capacity) output[0] = '\0';
    if (!source || source->struct_size != sizeof(*source) || !output || !capacity) return 1;
    const char* end = (const char*)memchr(source->module_path, '\0', sizeof(source->module_path));
    if (!end) return 1;
    const size_t length = (size_t)(end - source->module_path);
    if (length >= capacity) return 1;
    memcpy(output, source->module_path, length + 1);
    return 0;
}
#endif
