import Foundation

func recordingStatusText(_ recording: daw_recording, monitor: Bool) -> String {
    let mode = recording.loop_recording != 0 ? "Луп-запись" :
        (recording.target_track_id != 0 ? "Новый дубль" : "Запись")
    let passes = recording.loop_recording != 0 ? " · дублей \(recording.pass_count)" : ""
    let mon = monitor ? "MON вкл." : "MON выкл."
    return String(format: "● %@  %.1f с%@ · playback + mono / 48 кГц · %@",
                  mode, Double(recording.frames) / 48000, passes, mon)
}
