#include "daw.h"
#include <stddef.h>
_Static_assert(DAW_INSERT_HOSTING_STATUS_VERSION == 1, "hosting status ABI version");
_Static_assert(DAW_INSERT_HOSTING_MODE_IN_PROCESS == 1, "in-process ABI value");
_Static_assert(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS == 2, "out-of-process ABI value");
_Static_assert(DAW_INSERT_HOSTING_FORMAT_AUV2 == 1 && DAW_INSERT_HOSTING_FORMAT_AUV3 == 2 && DAW_INSERT_HOSTING_FORMAT_VST3 == 3, "hosting format ABI values");
_Static_assert(DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS == 1u && DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS == 2u, "hosting flag ABI values");
_Static_assert(offsetof(daw_insert_hosting_status, struct_size) == 0, "hosting status prefix");
_Static_assert(sizeof(daw_insert_hosting_status) == 20, "hosting status ABI size");
int main(void) {
    daw_session* session = daw_create();
    if (!session) return 1;
    daw_snapshot snapshot = {0}; snapshot.struct_size = sizeof(snapshot);
    daw_recording recording = {0}; recording.struct_size = sizeof(recording);
    daw_export_status export_status = {0}; export_status.struct_size = sizeof(export_status);
    daw_output_status output_status = {0}; output_status.struct_size = sizeof(output_status);
    daw_bus bus = {0}; bus.struct_size = sizeof(bus);
    daw_send send = {0}; send.struct_size = sizeof(send);
    daw_au_component component = {0}; component.struct_size = sizeof(component);
    daw_plugin plugin = {0}; plugin.struct_size = sizeof(plugin);
    daw_insert_hosting_status hosting = {0}; hosting.struct_size = sizeof(hosting);
    int result = daw_get_snapshot(session, &snapshot);
    result |= daw_get_recording(session, &recording);
    result |= daw_get_output_status(session, &output_status);
    result |= daw_set_loop(session, 0, 0, 0);
    daw_destroy(session);
    return result || snapshot.track_count != 0 || component.struct_size == 0 || plugin.struct_size == 0 || hosting.struct_size == 0;
}
