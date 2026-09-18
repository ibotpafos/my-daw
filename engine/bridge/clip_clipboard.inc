// Included once by daw.cpp after the session and transport helpers.
int daw_capture_clipboard(daw_session *s, uint64_t track, uint32_t midi, const uint32_t *indices,
                          uint32_t count, uint32_t cut, uint64_t revision) {
    return guard(s, [&] {
        if (midi > 1 || cut > 1 || count == 0 || count > 256 || !indices)
            throw daw::Error("Invalid clipboard selection");
        if (recordingActive(s) || s->midiRecorder)
            throw daw::Error("Stop recording and MIDI capture before clipboard edits");
        auto board =
            s->model.captureClips(track, {indices, indices + count}, midi != 0, cut != 0, revision);
        static_assert(std::is_nothrow_move_assignable_v<daw::ClipClipboard>);
        static_assert(std::is_nothrow_move_constructible_v<daw::ClipClipboard>);
        s->clipClipboard = std::move(board);
        if (cut)
            resetTransport(s);
    });
}
int daw_paste_clipboard(daw_session *s, uint64_t target, uint64_t start, uint64_t revision) {
    return guard(s, [&] {
        if (recordingActive(s) || s->midiRecorder)
            throw daw::Error("Stop recording and MIDI capture before clipboard edits");
        if (!s->clipClipboard)
            throw daw::Error("Clip clipboard is empty");
        s->model.pasteClips(*s->clipClipboard, target, start, revision);
        resetTransport(s);
    });
}
int daw_get_clipboard(daw_session *s, daw_clipboard_info *info) {
    return guard(s, [&] {
        if (!info || info->struct_size != sizeof(daw_clipboard_info))
            throw daw::Error("Clipboard ABI mismatch");
        *info = {sizeof(daw_clipboard_info), 0, 0, 0, 0};
        if (s->clipClipboard) {
            info->kind = s->clipClipboard->midi() ? 2 : 1;
            info->clip_count = s->clipClipboard->count();
            info->length = s->clipClipboard->length();
        }
    });
}
int daw_clear_clipboard(daw_session *s) {
    return guard(s, [&] {
        s->clipClipboard.reset();
    });
}
