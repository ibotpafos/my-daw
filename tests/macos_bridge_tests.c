#include "../apps/macos/DAWBridge.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks = 0;
static void expect(int condition) {
    ++checks;
    if (!condition) { fprintf(stderr, "macOS bridge assertion %u failed\n", checks); exit(1); }
}
int main(void) {
    daw_vst3_component descriptor = {0};
    descriptor.struct_size = sizeof(descriptor);
    char output[DAW_MACOS_VST3_PATH_CAPACITY];
    expect(daw_macos_copy_vst3_path(&descriptor, output, sizeof(output)) == 0 && output[0] == 0);
    strcpy(descriptor.module_path, "/Library/Audio/Plug-Ins/VST3/Example.vst3");
    expect(daw_macos_copy_vst3_path(&descriptor, output, sizeof(output)) == 0);
    expect(strcmp(output, descriptor.module_path) == 0);
    expect(daw_macos_copy_vst3_path(&descriptor, output, strlen(descriptor.module_path)) == 1 && output[0] == 0);
    expect(daw_macos_copy_vst3_path(&descriptor, output, strlen(descriptor.module_path) + 1) == 0);
    expect(daw_macos_copy_vst3_path(NULL, output, sizeof(output)) == 1 && output[0] == 0);
    expect(daw_macos_copy_vst3_path(&descriptor, NULL, sizeof(output)) == 1);
    expect(daw_macos_copy_vst3_path(&descriptor, output, 0) == 1);
    descriptor.struct_size = 0;
    expect(daw_macos_copy_vst3_path(&descriptor, output, sizeof(output)) == 1);
    descriptor.struct_size = sizeof(descriptor);
    memset(descriptor.module_path, 'x', sizeof(descriptor.module_path));
    expect(daw_macos_copy_vst3_path(&descriptor, output, sizeof(output)) == 1 && output[0] == 0);
    descriptor.module_path[sizeof(descriptor.module_path) - 1] = 0;
    expect(daw_macos_copy_vst3_path(&descriptor, output, sizeof(output)) == 0);
    expect(strlen(output) == sizeof(output) - 1 && memcmp(output, descriptor.module_path, sizeof(output)) == 0);
    puts("PASS: 12 bounded macOS C/Swift descriptor-copy assertions");
    return 0;
}
