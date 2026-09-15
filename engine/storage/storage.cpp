#include "domain/session.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <memory>
#include <cerrno>
#include <unistd.h>
namespace daw {
#ifdef __APPLE__
std::string replacementDirectory(const std::string& target);
#endif
namespace {
using DB = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
DB open(const std::string& path, int flags) {
    sqlite3* raw = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    DB db(raw, sqlite3_close);
    if (rc != SQLITE_OK) throw Error("Cannot open draft database");
    sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, 24 * 1024 * 1024);
    sqlite3_busy_timeout(raw, 1000);
    return db;
}
void sql(sqlite3* db, const char* text) {
    if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK) throw Error(sqlite3_errmsg(db));
}
Statement prepare(sqlite3* db, const char* text) {
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db, text, -1, &raw, nullptr);
    Statement statement(raw, sqlite3_finalize);
    if (rc != SQLITE_OK) throw Error(sqlite3_errmsg(db));
    return statement;
}
void done(sqlite3_stmt* s) { if (sqlite3_step(s) != SQLITE_DONE) throw Error("Draft write failed"); }
int64_t integer(sqlite3_stmt* s, int column) {
    if (sqlite3_column_type(s, column) != SQLITE_INTEGER) throw Error("Invalid integer in draft");
    return sqlite3_column_int64(s, column);
}
std::string string(sqlite3_stmt* s, int column) {
    if (sqlite3_column_type(s, column) != SQLITE_TEXT) throw Error("Invalid text in draft");
    auto p = reinterpret_cast<const char*>(sqlite3_column_text(s, column));
    if (!p) throw Error("Cannot read draft text");
    return {p, static_cast<size_t>(sqlite3_column_bytes(s, column))};
}
constexpr int appID = 1296323159; // Separate experimental format; not the .mydaw package.
}
void writeDraft(const State& state, const std::string& path, SaveObserver observer) {
    validate(state);
    if (path.empty()) throw Error("Choose a file path");
    auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";
    // Powerbox authorizes the selected file, not arbitrary siblings in Documents.
    // macOS provides a private staging directory on the destination volume.
    struct DirectoryCleanup { std::filesystem::path path; ~DirectoryCleanup(){ if(!path.empty()) { std::error_code ec; std::filesystem::remove(path, ec); } } } directoryCleanup;
#ifdef __APPLE__
    parent = replacementDirectory(path);
    directoryCleanup.path = parent;
#endif
    std::string pattern = (parent / ".mydaw-save-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end()); bytes.push_back(0);
    int fd = mkstemp(bytes.data());
    if (fd < 0) throw Error("Cannot create temporary draft snapshot");
    struct Descriptor { int value; ~Descriptor(){ if(value>=0) close(value); } } descriptor{fd};
    std::string temporary(bytes.data());
    struct Cleanup { std::string path; ~Cleanup(){ std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{temporary};
    if(observer) observer(1);
    {
        auto db = open(":memory:", SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
        sql(db.get(), "PRAGMA application_id=1296323159; PRAGMA user_version=18; BEGIN IMMEDIATE;"
            "CREATE TABLE metadata(singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL, next_id INTEGER NOT NULL, master_gain REAL NOT NULL);"
            "CREATE TABLE tracks(position INTEGER PRIMARY KEY, id INTEGER UNIQUE NOT NULL, name TEXT NOT NULL, gain REAL NOT NULL, pcm BLOB, pan REAL NOT NULL, muted INTEGER NOT NULL CHECK(muted IN(0,1)), solo INTEGER NOT NULL CHECK(solo IN(0,1)), base_start INTEGER NOT NULL, output_bus INTEGER NOT NULL);"
            "CREATE TABLE takes(track_id INTEGER NOT NULL, position INTEGER NOT NULL, name TEXT NOT NULL, take_start INTEGER NOT NULL, pcm BLOB NOT NULL, PRIMARY KEY(track_id,position));"
            "CREATE TABLE regions(track_id INTEGER NOT NULL, position INTEGER NOT NULL, clip_start INTEGER NOT NULL, source_offset INTEGER NOT NULL, clip_length INTEGER NOT NULL, fade_in INTEGER NOT NULL, fade_out INTEGER NOT NULL, take_index INTEGER NOT NULL, PRIMARY KEY(track_id,position));"
            "CREATE TABLE buses(position INTEGER PRIMARY KEY, id INTEGER UNIQUE NOT NULL, name TEXT NOT NULL, gain REAL NOT NULL, pan REAL NOT NULL, muted INTEGER NOT NULL CHECK(muted IN(0,1)), output_bus INTEGER NOT NULL);"
            "CREATE TABLE sends(track_id INTEGER NOT NULL, position INTEGER NOT NULL, bus_id INTEGER NOT NULL, gain REAL NOT NULL, pre_fader INTEGER NOT NULL CHECK(pre_fader IN(0,1)), PRIMARY KEY(track_id,position));"
            "CREATE TABLE track_volume_automation(track_id INTEGER NOT NULL, position INTEGER NOT NULL, frame INTEGER NOT NULL, gain REAL NOT NULL, PRIMARY KEY(track_id,position), UNIQUE(track_id,frame));"
            "CREATE TABLE track_pan_automation(track_id INTEGER NOT NULL, position INTEGER NOT NULL, frame INTEGER NOT NULL, pan REAL NOT NULL, PRIMARY KEY(track_id,position), UNIQUE(track_id,frame));"
            "CREATE TABLE bus_gain_automation(bus_id INTEGER NOT NULL, position INTEGER NOT NULL, frame INTEGER NOT NULL, gain REAL NOT NULL, PRIMARY KEY(bus_id,position), UNIQUE(bus_id,frame));"
            "CREATE TABLE master_gain_automation(position INTEGER PRIMARY KEY, frame INTEGER UNIQUE NOT NULL, gain REAL NOT NULL);"
            "CREATE TABLE channel_plugins(owner_kind INTEGER NOT NULL CHECK(owner_kind IN(1,2,3)), owner_id INTEGER NOT NULL, position INTEGER NOT NULL, id INTEGER UNIQUE NOT NULL, type INTEGER NOT NULL, subtype INTEGER NOT NULL, manufacturer INTEGER NOT NULL, name TEXT NOT NULL, bypassed INTEGER NOT NULL CHECK(bypassed IN(0,1)), latency_frames INTEGER NOT NULL, state BLOB NOT NULL, hosting_mode INTEGER NOT NULL CHECK(hosting_mode IN(1,2)), PRIMARY KEY(owner_kind,owner_id,position));"
            "CREATE TABLE plugin_parameter_automation(plugin_id INTEGER NOT NULL, parameter_id INTEGER NOT NULL, lane_position INTEGER NOT NULL, point_position INTEGER NOT NULL, frame INTEGER NOT NULL, normalized_value REAL NOT NULL, name TEXT NOT NULL, PRIMARY KEY(plugin_id,lane_position,point_position), UNIQUE(plugin_id,parameter_id,frame));"
            "CREATE TABLE midi_clips(track_id INTEGER NOT NULL, position INTEGER NOT NULL, start INTEGER NOT NULL, length INTEGER NOT NULL, lane INTEGER NOT NULL, PRIMARY KEY(track_id,position));"
            "CREATE TABLE midi_notes(track_id INTEGER NOT NULL, clip_position INTEGER NOT NULL, position INTEGER NOT NULL, start INTEGER NOT NULL, length INTEGER NOT NULL, pitch INTEGER NOT NULL, channel INTEGER NOT NULL, velocity INTEGER NOT NULL, PRIMARY KEY(track_id,clip_position,position));"
            "CREATE TABLE tempo_points(frame INTEGER PRIMARY KEY, bpm REAL NOT NULL);"
            "CREATE TABLE time_signature_points(frame INTEGER PRIMARY KEY, numerator INTEGER NOT NULL, denominator INTEGER NOT NULL);"
            "CREATE TABLE markers(frame INTEGER PRIMARY KEY, name TEXT NOT NULL);");
        auto meta = prepare(db.get(), "INSERT INTO metadata VALUES(1,?,?,?)");
        sqlite3_bind_int64(meta.get(), 1, static_cast<int64_t>(state.revision));
        sqlite3_bind_int64(meta.get(), 2, static_cast<int64_t>(state.nextID));sqlite3_bind_double(meta.get(),3,state.masterGain); done(meta.get());
        auto row = prepare(db.get(), "INSERT INTO tracks VALUES(?,?,?,?,?,?,?,?,?,?)");
        auto takeRow=prepare(db.get(),"INSERT INTO takes VALUES(?,?,?,?,?)");
        auto regionRow = prepare(db.get(), "INSERT INTO regions VALUES(?,?,?,?,?,?,?,?)");
        int position = 0;
        for (const auto& t : state.tracks) {
            sqlite3_reset(row.get()); sqlite3_clear_bindings(row.get());
            sqlite3_bind_int(row.get(), 1, position++); sqlite3_bind_int64(row.get(), 2, static_cast<int64_t>(t.id));
            if (sqlite3_bind_text(row.get(), 3, t.name.data(), static_cast<int>(t.name.size()), SQLITE_TRANSIENT) != SQLITE_OK) throw Error("Cannot bind draft name");
            sqlite3_bind_double(row.get(), 4, t.gain);
            if(t.audio) { auto pcm=encodePCM(*t.audio); if(sqlite3_bind_blob(row.get(),5,pcm.data(),static_cast<int>(pcm.size()),SQLITE_TRANSIENT)!=SQLITE_OK) throw Error("Cannot bind audio"); }
            sqlite3_bind_double(row.get(),6,t.pan);sqlite3_bind_int(row.get(),7,t.muted);sqlite3_bind_int(row.get(),8,t.solo);sqlite3_bind_int64(row.get(),9,static_cast<int64_t>(t.baseStart));sqlite3_bind_int64(row.get(),10,static_cast<int64_t>(t.outputBus));
            done(row.get());
            for(size_t takePosition=0;takePosition<t.takes.size();++takePosition){const auto& take=t.takes[takePosition];sqlite3_reset(takeRow.get());sqlite3_clear_bindings(takeRow.get());sqlite3_bind_int64(takeRow.get(),1,static_cast<int64_t>(t.id));sqlite3_bind_int64(takeRow.get(),2,static_cast<int64_t>(takePosition));sqlite3_bind_text(takeRow.get(),3,take.name.data(),static_cast<int>(take.name.size()),SQLITE_TRANSIENT);sqlite3_bind_int64(takeRow.get(),4,static_cast<int64_t>(take.start));auto pcm=encodePCM(*take.audio);if(sqlite3_bind_blob(takeRow.get(),5,pcm.data(),static_cast<int>(pcm.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw Error("Cannot bind take audio");done(takeRow.get());}
            for(size_t clipPosition=0;clipPosition<t.regions.size();++clipPosition) {
                const auto& region=t.regions[clipPosition]; sqlite3_reset(regionRow.get()); sqlite3_clear_bindings(regionRow.get());
                sqlite3_bind_int64(regionRow.get(),1,static_cast<int64_t>(t.id)); sqlite3_bind_int64(regionRow.get(),2,static_cast<int64_t>(clipPosition));
                sqlite3_bind_int64(regionRow.get(),3,static_cast<int64_t>(region.start)); sqlite3_bind_int64(regionRow.get(),4,static_cast<int64_t>(region.sourceOffset)); sqlite3_bind_int64(regionRow.get(),5,static_cast<int64_t>(region.length));
                sqlite3_bind_int64(regionRow.get(),6,static_cast<int64_t>(region.fadeIn)); sqlite3_bind_int64(regionRow.get(),7,static_cast<int64_t>(region.fadeOut));sqlite3_bind_int64(regionRow.get(),8,region.take); done(regionRow.get());
            }
        }
        auto busRow=prepare(db.get(),"INSERT INTO buses VALUES(?,?,?,?,?,?,?)");
        for(size_t position=0;position<state.buses.size();++position){const auto& bus=state.buses[position];sqlite3_reset(busRow.get());sqlite3_clear_bindings(busRow.get());sqlite3_bind_int64(busRow.get(),1,static_cast<int64_t>(position));sqlite3_bind_int64(busRow.get(),2,static_cast<int64_t>(bus.id));sqlite3_bind_text(busRow.get(),3,bus.name.data(),static_cast<int>(bus.name.size()),SQLITE_TRANSIENT);sqlite3_bind_double(busRow.get(),4,bus.gain);sqlite3_bind_double(busRow.get(),5,bus.pan);sqlite3_bind_int(busRow.get(),6,bus.muted);sqlite3_bind_int64(busRow.get(),7,static_cast<int64_t>(bus.outputBus));done(busRow.get());}
        auto sendRow=prepare(db.get(),"INSERT INTO sends VALUES(?,?,?,?,?)");
        for(const auto& track:state.tracks)for(size_t position=0;position<track.sends.size();++position){const auto& send=track.sends[position];sqlite3_reset(sendRow.get());sqlite3_clear_bindings(sendRow.get());sqlite3_bind_int64(sendRow.get(),1,static_cast<int64_t>(track.id));sqlite3_bind_int64(sendRow.get(),2,static_cast<int64_t>(position));sqlite3_bind_int64(sendRow.get(),3,static_cast<int64_t>(send.bus));sqlite3_bind_double(sendRow.get(),4,send.gain);sqlite3_bind_int(sendRow.get(),5,send.preFader);done(sendRow.get());}
        auto automationRow=prepare(db.get(),"INSERT INTO track_volume_automation VALUES(?,?,?,?)");
        for(const auto& track:state.tracks)for(size_t position=0;position<track.volumeAutomation.size();++position){const auto& point=track.volumeAutomation[position];sqlite3_reset(automationRow.get());sqlite3_clear_bindings(automationRow.get());sqlite3_bind_int64(automationRow.get(),1,static_cast<int64_t>(track.id));sqlite3_bind_int64(automationRow.get(),2,static_cast<int64_t>(position));sqlite3_bind_int64(automationRow.get(),3,static_cast<int64_t>(point.frame));sqlite3_bind_double(automationRow.get(),4,point.gainDb);done(automationRow.get());}
        auto panAutomationRow=prepare(db.get(),"INSERT INTO track_pan_automation VALUES(?,?,?,?)");
        for(const auto& track:state.tracks)for(size_t position=0;position<track.panAutomation.size();++position){const auto& point=track.panAutomation[position];sqlite3_reset(panAutomationRow.get());sqlite3_clear_bindings(panAutomationRow.get());sqlite3_bind_int64(panAutomationRow.get(),1,static_cast<int64_t>(track.id));sqlite3_bind_int64(panAutomationRow.get(),2,static_cast<int64_t>(position));sqlite3_bind_int64(panAutomationRow.get(),3,static_cast<int64_t>(point.frame));sqlite3_bind_double(panAutomationRow.get(),4,point.gainDb);done(panAutomationRow.get());}
        auto busAutomationRow=prepare(db.get(),"INSERT INTO bus_gain_automation VALUES(?,?,?,?)");
        for(const auto& bus:state.buses)for(size_t position=0;position<bus.gainAutomation.size();++position){const auto& point=bus.gainAutomation[position];sqlite3_reset(busAutomationRow.get());sqlite3_clear_bindings(busAutomationRow.get());sqlite3_bind_int64(busAutomationRow.get(),1,static_cast<int64_t>(bus.id));sqlite3_bind_int64(busAutomationRow.get(),2,static_cast<int64_t>(position));sqlite3_bind_int64(busAutomationRow.get(),3,static_cast<int64_t>(point.frame));sqlite3_bind_double(busAutomationRow.get(),4,point.gainDb);done(busAutomationRow.get());}
        auto masterAutomationRow=prepare(db.get(),"INSERT INTO master_gain_automation VALUES(?,?,?)");
        for(size_t position=0;position<state.masterGainAutomation.size();++position){const auto& point=state.masterGainAutomation[position];sqlite3_reset(masterAutomationRow.get());sqlite3_clear_bindings(masterAutomationRow.get());sqlite3_bind_int64(masterAutomationRow.get(),1,static_cast<int64_t>(position));sqlite3_bind_int64(masterAutomationRow.get(),2,static_cast<int64_t>(point.frame));sqlite3_bind_double(masterAutomationRow.get(),3,point.gainDb);done(masterAutomationRow.get());}
        auto pluginRow=prepare(db.get(),"INSERT INTO channel_plugins VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        const auto writePlugins=[&](int ownerKind,uint64_t ownerID,const std::vector<PluginInsert>& inserts){for(size_t position=0;position<inserts.size();++position){const auto& plugin=inserts[position];sqlite3_reset(pluginRow.get());sqlite3_clear_bindings(pluginRow.get());sqlite3_bind_int(pluginRow.get(),1,ownerKind);sqlite3_bind_int64(pluginRow.get(),2,static_cast<int64_t>(ownerID));sqlite3_bind_int64(pluginRow.get(),3,static_cast<int64_t>(position));sqlite3_bind_int64(pluginRow.get(),4,static_cast<int64_t>(plugin.id));sqlite3_bind_int64(pluginRow.get(),5,plugin.type);sqlite3_bind_int64(pluginRow.get(),6,plugin.subtype);sqlite3_bind_int64(pluginRow.get(),7,plugin.manufacturer);sqlite3_bind_text(pluginRow.get(),8,plugin.name.data(),static_cast<int>(plugin.name.size()),SQLITE_TRANSIENT);sqlite3_bind_int(pluginRow.get(),9,plugin.bypassed);sqlite3_bind_int64(pluginRow.get(),10,plugin.latencyFrames);if(plugin.state.empty())sqlite3_bind_zeroblob(pluginRow.get(),11,0);else if(sqlite3_bind_blob(pluginRow.get(),11,plugin.state.data(),static_cast<int>(plugin.state.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw Error("Cannot bind plug-in state");sqlite3_bind_int(pluginRow.get(),12,static_cast<int>(plugin.hostingMode));done(pluginRow.get());}};
        for(const auto& track:state.tracks)writePlugins(1,track.id,track.inserts);
        for(const auto& bus:state.buses)writePlugins(2,bus.id,bus.inserts);
        writePlugins(3,0,state.masterInserts);
        auto parameterAutomationRow=prepare(db.get(),"INSERT INTO plugin_parameter_automation VALUES(?,?,?,?,?,?,?)");
        const auto writeParameterAutomation=[&](const std::vector<PluginInsert>& inserts){for(const auto& plugin:inserts)for(size_t lanePosition=0;lanePosition<plugin.parameterAutomation.size();++lanePosition){const auto& lane=plugin.parameterAutomation[lanePosition];for(size_t pointPosition=0;pointPosition<lane.points.size();++pointPosition){const auto& point=lane.points[pointPosition];sqlite3_reset(parameterAutomationRow.get());sqlite3_clear_bindings(parameterAutomationRow.get());sqlite3_bind_int64(parameterAutomationRow.get(),1,static_cast<int64_t>(plugin.id));sqlite3_bind_int64(parameterAutomationRow.get(),2,lane.parameterID);sqlite3_bind_int64(parameterAutomationRow.get(),3,static_cast<int64_t>(lanePosition));sqlite3_bind_int64(parameterAutomationRow.get(),4,static_cast<int64_t>(pointPosition));sqlite3_bind_int64(parameterAutomationRow.get(),5,static_cast<int64_t>(point.frame));sqlite3_bind_double(parameterAutomationRow.get(),6,point.normalizedValue);sqlite3_bind_text(parameterAutomationRow.get(),7,lane.name.data(),static_cast<int>(lane.name.size()),SQLITE_TRANSIENT);done(parameterAutomationRow.get());}}};
        for(const auto& track:state.tracks)writeParameterAutomation(track.inserts);for(const auto& bus:state.buses)writeParameterAutomation(bus.inserts);writeParameterAutomation(state.masterInserts);
        auto midiClipRow=prepare(db.get(),"INSERT INTO midi_clips VALUES(?,?,?,?,?)");
        auto midiNoteRow=prepare(db.get(),"INSERT INTO midi_notes VALUES(?,?,?,?,?,?,?,?)");
        for(const auto& track:state.tracks)for(size_t position=0;position<track.midiClips.size();++position){const auto& clip=track.midiClips[position];sqlite3_reset(midiClipRow.get());sqlite3_clear_bindings(midiClipRow.get());sqlite3_bind_int64(midiClipRow.get(),1,static_cast<int64_t>(track.id));sqlite3_bind_int64(midiClipRow.get(),2,static_cast<int64_t>(position));sqlite3_bind_int64(midiClipRow.get(),3,static_cast<int64_t>(clip.start));sqlite3_bind_int64(midiClipRow.get(),4,static_cast<int64_t>(clip.length));sqlite3_bind_int64(midiClipRow.get(),5,clip.track);done(midiClipRow.get());for(size_t notePosition=0;notePosition<clip.notes.size();++notePosition){const auto& note=clip.notes[notePosition];sqlite3_reset(midiNoteRow.get());sqlite3_clear_bindings(midiNoteRow.get());sqlite3_bind_int64(midiNoteRow.get(),1,static_cast<int64_t>(track.id));sqlite3_bind_int64(midiNoteRow.get(),2,static_cast<int64_t>(position));sqlite3_bind_int64(midiNoteRow.get(),3,static_cast<int64_t>(notePosition));sqlite3_bind_int64(midiNoteRow.get(),4,static_cast<int64_t>(note.start));sqlite3_bind_int64(midiNoteRow.get(),5,static_cast<int64_t>(note.length));sqlite3_bind_int64(midiNoteRow.get(),6,note.pitch);sqlite3_bind_int64(midiNoteRow.get(),7,note.channel);sqlite3_bind_int64(midiNoteRow.get(),8,note.velocity);done(midiNoteRow.get());}}
        auto tempoRow=prepare(db.get(),"INSERT INTO tempo_points VALUES(?,?)");
        for(const auto& point:state.tempo){sqlite3_reset(tempoRow.get());sqlite3_clear_bindings(tempoRow.get());sqlite3_bind_int64(tempoRow.get(),1,static_cast<int64_t>(point.frame));sqlite3_bind_double(tempoRow.get(),2,point.bpm);done(tempoRow.get());}
        auto timeSignatureRow=prepare(db.get(),"INSERT INTO time_signature_points VALUES(?,?,?)");
        for(const auto& point:state.timeSignatures){sqlite3_reset(timeSignatureRow.get());sqlite3_clear_bindings(timeSignatureRow.get());sqlite3_bind_int64(timeSignatureRow.get(),1,static_cast<int64_t>(point.frame));sqlite3_bind_int64(timeSignatureRow.get(),2,point.numerator);sqlite3_bind_int64(timeSignatureRow.get(),3,point.denominator);done(timeSignatureRow.get());}
        auto markerRow=prepare(db.get(),"INSERT INTO markers VALUES(?,?)");
        for(const auto& marker:state.markers){sqlite3_reset(markerRow.get());sqlite3_clear_bindings(markerRow.get());sqlite3_bind_int64(markerRow.get(),1,static_cast<int64_t>(marker.frame));if(sqlite3_bind_text(markerRow.get(),2,marker.name.data(),static_cast<int>(marker.name.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw Error("Cannot bind marker name");done(markerRow.get());}
        sql(db.get(), "COMMIT");
        sqlite3_int64 size=0;
        std::unique_ptr<unsigned char,decltype(&sqlite3_free)> image(sqlite3_serialize(db.get(),"main",&size,0),sqlite3_free);
        if(!image || size<=0 || size>72*1024*1024) throw Error("Cannot serialize bounded draft database");
        size_t written=0;
        while(written<static_cast<size_t>(size)) {
            auto count=write(fd,image.get()+written,static_cast<size_t>(size)-written);
            if(count<0 && errno==EINTR) continue;
            if(count<=0) throw Error("Cannot write complete draft snapshot");
            written+=static_cast<size_t>(count);
        }
        int syncResult; do { syncResult=fsync(fd); } while(syncResult<0 && errno==EINTR);
        if(syncResult<0) throw Error("Cannot sync draft snapshot");
        int closeResult=close(fd); descriptor.value=-1;
        if(closeResult<0) throw Error("Cannot close draft snapshot");
    }
    // POSIX rename atomically replaces an existing snapshot on the same filesystem.
    // This is not a claim of power-loss recovery or multi-writer protection.
    if(observer) observer(2);
    std::filesystem::rename(temporary, path);
    if(observer) observer(3);
}
State readDraft(const std::string& path) {
    if (std::filesystem::file_size(path) > 72 * 1024 * 1024) throw Error("Draft exceeds 72 MiB limit");
    auto db = open(path, SQLITE_OPEN_READONLY);
    sql(db.get(), "PRAGMA trusted_schema=OFF; BEGIN");
    auto app = prepare(db.get(), "PRAGMA application_id");
    if (sqlite3_step(app.get()) != SQLITE_ROW || integer(app.get(), 0) != appID) throw Error("This is not a My DAW draft");
    auto version = prepare(db.get(), "PRAGMA user_version");
    if (sqlite3_step(version.get()) != SQLITE_ROW) throw Error("Missing draft version");
    auto formatVersion=integer(version.get(),0);
    if(formatVersion<1 || formatVersion>18) throw Error("Unsupported draft version");
    auto integrity = prepare(db.get(), "PRAGMA quick_check");
    if (sqlite3_step(integrity.get()) != SQLITE_ROW || string(integrity.get(), 0) != "ok") throw Error("Damaged draft database");
    auto meta = prepare(db.get(),formatVersion>=7?"SELECT singleton,revision,next_id,master_gain FROM metadata":"SELECT singleton,revision,next_id,0 FROM metadata");
    if (sqlite3_step(meta.get()) != SQLITE_ROW || integer(meta.get(), 0) != 1) throw Error("Missing draft metadata");
    if(sqlite3_column_type(meta.get(),3)!=SQLITE_FLOAT&&sqlite3_column_type(meta.get(),3)!=SQLITE_INTEGER)throw Error("Invalid master gain");
    State state; state.revision = static_cast<uint64_t>(integer(meta.get(), 1)); state.nextID = static_cast<uint64_t>(integer(meta.get(), 2));state.masterGain=sqlite3_column_double(meta.get(),3);
    if (sqlite3_step(meta.get()) != SQLITE_DONE) throw Error("Ambiguous draft metadata");
    std::string trackQuery;
    if(formatVersion==1)trackQuery="SELECT position,id,name,gain,NULL,0,0,0,0,0 FROM tracks ORDER BY position";
    else if(formatVersion==2)trackQuery="SELECT position,id,name,gain,pcm,0,0,0,0,0 FROM tracks ORDER BY position";
    else if(formatVersion==3)trackQuery="SELECT position,id,name,gain,pcm,clip_start,source_offset,clip_length,0,0 FROM tracks ORDER BY position";
    else if(formatVersion<7)trackQuery="SELECT position,id,name,gain,pcm,0,0,0,0,0 FROM tracks ORDER BY position";
    else if(formatVersion==7)trackQuery="SELECT position,id,name,gain,pcm,pan,muted,solo,0,0 FROM tracks ORDER BY position";
    else if(formatVersion==8)trackQuery="SELECT position,id,name,gain,pcm,pan,muted,solo,base_start,0 FROM tracks ORDER BY position";
    else trackQuery="SELECT position,id,name,gain,pcm,pan,muted,solo,base_start,output_bus FROM tracks ORDER BY position";
    auto row = prepare(db.get(),trackQuery.c_str());
    int rc; size_t audioBytes=0;
    while ((rc = sqlite3_step(row.get())) == SQLITE_ROW) {
        if (state.tracks.size() >= 256 || integer(row.get(), 0) != static_cast<int64_t>(state.tracks.size())) throw Error("Invalid draft track order");
        int type = sqlite3_column_type(row.get(), 3);
        if (type != SQLITE_FLOAT && type != SQLITE_INTEGER) throw Error("Invalid draft gain");
        if(formatVersion!=3&&(sqlite3_column_type(row.get(),5)!=SQLITE_FLOAT&&sqlite3_column_type(row.get(),5)!=SQLITE_INTEGER))throw Error("Invalid draft pan");
        const auto muted=formatVersion==3?false:integer(row.get(),6);const auto solo=formatVersion==3?false:integer(row.get(),7);if(muted<0||muted>1||solo<0||solo>1)throw Error("Invalid mute or solo state");
        state.tracks.push_back({static_cast<uint64_t>(integer(row.get(), 1)), string(row.get(), 2), sqlite3_column_double(row.get(), 3), {}, {},formatVersion==3?0:sqlite3_column_double(row.get(),5),muted!=0,solo!=0,formatVersion>=8?static_cast<uint64_t>(integer(row.get(),8)):0,{},formatVersion>=9?static_cast<uint64_t>(integer(row.get(),9)):0,{},{},{},{},{}});
        if(sqlite3_column_type(row.get(),4)!=SQLITE_NULL) {
            if(sqlite3_column_type(row.get(),4)!=SQLITE_BLOB) throw Error("Invalid audio storage");
            size_t size=static_cast<size_t>(sqlite3_column_bytes(row.get(),4)); audioBytes+=size;
            if(audioBytes>64*1024*1024) throw Error("Draft audio exceeds memory limit");
            auto bytes=static_cast<const unsigned char*>(sqlite3_column_blob(row.get(),4));
            if(!bytes) throw Error("Missing PCM data");
            state.tracks.back().audio=decodePCM({bytes,size});
            if(formatVersion<3) state.tracks.back().regions.push_back({0,0,state.tracks.back().audio->frames()});
        }
        if(formatVersion==3 && state.tracks.back().audio) {
            auto& track=state.tracks.back(); track.regions.push_back({static_cast<uint64_t>(integer(row.get(),5)),static_cast<uint64_t>(integer(row.get(),6)),static_cast<uint64_t>(integer(row.get(),7))});
        }
    }
    if (rc != SQLITE_DONE) throw Error("Cannot read draft tracks");
    if(formatVersion>=8){auto takes=prepare(db.get(),"SELECT track_id,position,name,take_start,pcm FROM takes ORDER BY track_id,position");while((rc=sqlite3_step(takes.get()))==SQLITE_ROW){const auto trackID=static_cast<uint64_t>(integer(takes.get(),0));auto track=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& t){return t.id==trackID;});if(track==state.tracks.end()||integer(takes.get(),1)!=static_cast<int64_t>(track->takes.size())||sqlite3_column_type(takes.get(),4)!=SQLITE_BLOB)throw Error("Invalid draft take order");const auto size=static_cast<size_t>(sqlite3_column_bytes(takes.get(),4));audioBytes+=size;if(audioBytes>64*1024*1024)throw Error("Draft audio exceeds memory limit");const auto bytes=static_cast<const unsigned char*>(sqlite3_column_blob(takes.get(),4));if(!bytes)throw Error("Missing take PCM data");track->takes.push_back({string(takes.get(),2),static_cast<uint64_t>(integer(takes.get(),3)),decodePCM({bytes,size})});}if(rc!=SQLITE_DONE)throw Error("Cannot read draft takes");}
    if(formatVersion>=4) {
        auto regions=prepare(db.get(),formatVersion==4 ? "SELECT track_id,position,clip_start,source_offset,clip_length,0,0,0 FROM regions ORDER BY track_id,position" : (formatVersion<8?"SELECT track_id,position,clip_start,source_offset,clip_length,fade_in,fade_out,0 FROM regions ORDER BY track_id,position":"SELECT track_id,position,clip_start,source_offset,clip_length,fade_in,fade_out,take_index FROM regions ORDER BY track_id,position"));
        while((rc=sqlite3_step(regions.get()))==SQLITE_ROW) {
            const auto trackID=static_cast<uint64_t>(integer(regions.get(),0));
            auto track=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& t){return t.id==trackID;});
            if(track==state.tracks.end() || integer(regions.get(),1)!=static_cast<int64_t>(track->regions.size())) throw Error("Invalid draft clip order");
            track->regions.push_back({static_cast<uint64_t>(integer(regions.get(),2)),static_cast<uint64_t>(integer(regions.get(),3)),static_cast<uint64_t>(integer(regions.get(),4)),static_cast<uint64_t>(integer(regions.get(),5)),static_cast<uint64_t>(integer(regions.get(),6)),static_cast<uint32_t>(integer(regions.get(),7))});
        }
        if(rc!=SQLITE_DONE) throw Error("Cannot read draft clips");
    }
    if(formatVersion<8)for(auto& track:state.tracks)if(track.audio&&!track.regions.empty()){uint64_t inferred=UINT64_MAX;for(const auto& region:track.regions)if(region.start>=region.sourceOffset)inferred=std::min(inferred,region.start-region.sourceOffset);track.baseStart=inferred==UINT64_MAX?0:inferred;}
    if(formatVersion>=9){
        auto buses=prepare(db.get(),"SELECT position,id,name,gain,pan,muted,output_bus FROM buses ORDER BY position");
        while((rc=sqlite3_step(buses.get()))==SQLITE_ROW){if(integer(buses.get(),0)!=static_cast<int64_t>(state.buses.size()))throw Error("Invalid draft bus order");const auto muted=integer(buses.get(),5);if(muted<0||muted>1)throw Error("Invalid bus mute state");state.buses.push_back({static_cast<uint64_t>(integer(buses.get(),1)),string(buses.get(),2),sqlite3_column_double(buses.get(),3),sqlite3_column_double(buses.get(),4),muted!=0,static_cast<uint64_t>(integer(buses.get(),6)),{},{}});}
        if(rc!=SQLITE_DONE)throw Error("Cannot read draft buses");
        auto sends=prepare(db.get(),"SELECT track_id,position,bus_id,gain,pre_fader FROM sends ORDER BY track_id,position");
        while((rc=sqlite3_step(sends.get()))==SQLITE_ROW){const auto trackID=static_cast<uint64_t>(integer(sends.get(),0));auto track=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& item){return item.id==trackID;});if(track==state.tracks.end()||integer(sends.get(),1)!=static_cast<int64_t>(track->sends.size()))throw Error("Invalid draft send order");const auto pre=integer(sends.get(),4);if(pre<0||pre>1)throw Error("Invalid send tap");track->sends.push_back({static_cast<uint64_t>(integer(sends.get(),2)),sqlite3_column_double(sends.get(),3),pre!=0});}
        if(rc!=SQLITE_DONE)throw Error("Cannot read draft sends");
    }
    if(formatVersion>=11){
        auto automation=prepare(db.get(),"SELECT track_id,position,frame,gain FROM track_volume_automation ORDER BY track_id,position");
        while((rc=sqlite3_step(automation.get()))==SQLITE_ROW){
            const auto trackID=static_cast<uint64_t>(integer(automation.get(),0));
            auto track=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& item){return item.id==trackID;});
            if(track==state.tracks.end()||integer(automation.get(),1)!=static_cast<int64_t>(track->volumeAutomation.size()))throw Error("Invalid track automation order");
            if(sqlite3_column_type(automation.get(),3)!=SQLITE_FLOAT&&sqlite3_column_type(automation.get(),3)!=SQLITE_INTEGER)throw Error("Invalid automation gain");
            track->volumeAutomation.push_back({static_cast<uint64_t>(integer(automation.get(),2)),sqlite3_column_double(automation.get(),3)});
        }
        if(rc!=SQLITE_DONE)throw Error("Cannot read track automation");
    }
    if(formatVersion>=12){
        auto readTrackAutomation=[&](const char* query,std::vector<AutomationPoint> Track::*lane,const char* invalid){
            auto automation=prepare(db.get(),query);
            while((rc=sqlite3_step(automation.get()))==SQLITE_ROW){const auto trackID=static_cast<uint64_t>(integer(automation.get(),0));auto track=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& item){return item.id==trackID;});if(track==state.tracks.end()||integer(automation.get(),1)!=static_cast<int64_t>(((*track).*lane).size()))throw Error(invalid);if(sqlite3_column_type(automation.get(),3)!=SQLITE_FLOAT&&sqlite3_column_type(automation.get(),3)!=SQLITE_INTEGER)throw Error("Invalid automation value");((*track).*lane).push_back({static_cast<uint64_t>(integer(automation.get(),2)),sqlite3_column_double(automation.get(),3)});}
            if(rc!=SQLITE_DONE)throw Error(invalid);
        };
        readTrackAutomation("SELECT track_id,position,frame,pan FROM track_pan_automation ORDER BY track_id,position",&Track::panAutomation,"Invalid track pan automation");
        auto busAutomation=prepare(db.get(),"SELECT bus_id,position,frame,gain FROM bus_gain_automation ORDER BY bus_id,position");
        while((rc=sqlite3_step(busAutomation.get()))==SQLITE_ROW){const auto busID=static_cast<uint64_t>(integer(busAutomation.get(),0));auto bus=std::find_if(state.buses.begin(),state.buses.end(),[&](const auto& item){return item.id==busID;});if(bus==state.buses.end()||integer(busAutomation.get(),1)!=static_cast<int64_t>(bus->gainAutomation.size()))throw Error("Invalid bus gain automation");if(sqlite3_column_type(busAutomation.get(),3)!=SQLITE_FLOAT&&sqlite3_column_type(busAutomation.get(),3)!=SQLITE_INTEGER)throw Error("Invalid automation gain");bus->gainAutomation.push_back({static_cast<uint64_t>(integer(busAutomation.get(),2)),sqlite3_column_double(busAutomation.get(),3)});}
        if(rc!=SQLITE_DONE)throw Error("Cannot read bus gain automation");
        auto masterAutomation=prepare(db.get(),"SELECT position,frame,gain FROM master_gain_automation ORDER BY position");
        while((rc=sqlite3_step(masterAutomation.get()))==SQLITE_ROW){if(integer(masterAutomation.get(),0)!=static_cast<int64_t>(state.masterGainAutomation.size()))throw Error("Invalid master gain automation order");if(sqlite3_column_type(masterAutomation.get(),2)!=SQLITE_FLOAT&&sqlite3_column_type(masterAutomation.get(),2)!=SQLITE_INTEGER)throw Error("Invalid automation gain");state.masterGainAutomation.push_back({static_cast<uint64_t>(integer(masterAutomation.get(),1)),sqlite3_column_double(masterAutomation.get(),2)});}
        if(rc!=SQLITE_DONE)throw Error("Cannot read master gain automation");
    }
    if(formatVersion>=13){
        auto plugins=prepare(db.get(),formatVersion>=15?"SELECT owner_kind,owner_id,position,id,type,subtype,manufacturer,name,bypassed,latency_frames,state,hosting_mode FROM channel_plugins ORDER BY owner_kind,owner_id,position":"SELECT owner_kind,owner_id,position,id,type,subtype,manufacturer,name,bypassed,latency_frames,state,1 FROM channel_plugins ORDER BY owner_kind,owner_id,position");size_t stateBytes=0;
        while((rc=sqlite3_step(plugins.get()))==SQLITE_ROW){const auto kind=integer(plugins.get(),0);const auto ownerID=static_cast<uint64_t>(integer(plugins.get(),1));std::vector<PluginInsert>* owner=nullptr;if(kind==1){auto track=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& item){return item.id==ownerID;});if(track==state.tracks.end())throw Error("Track plug-in owner not found");owner=&track->inserts;}else if(kind==2){auto bus=std::find_if(state.buses.begin(),state.buses.end(),[&](const auto& item){return item.id==ownerID;});if(bus==state.buses.end())throw Error("Bus plug-in owner not found");owner=&bus->inserts;}else if(kind==3){if(ownerID)throw Error("Master plug-in owner must be zero");owner=&state.masterInserts;}else throw Error("Invalid plug-in owner kind");if(integer(plugins.get(),2)!=static_cast<int64_t>(owner->size()))throw Error("Invalid channel plug-in order");const auto bypassed=integer(plugins.get(),8),latency=integer(plugins.get(),9),hostingMode=integer(plugins.get(),11);if(bypassed<0||bypassed>1||latency<0||latency>48000*10||(hostingMode!=static_cast<int64_t>(PluginHostingMode::InProcess)&&hostingMode!=static_cast<int64_t>(PluginHostingMode::OutOfProcess))||sqlite3_column_type(plugins.get(),10)!=SQLITE_BLOB)throw Error("Invalid channel plug-in state");const auto type=static_cast<uint32_t>(integer(plugins.get(),4)),subtype=static_cast<uint32_t>(integer(plugins.get(),5)),manufacturer=static_cast<uint32_t>(integer(plugins.get(),6));const auto vst3=type==kVst3PluginComponentSentinel&&subtype==kVst3PluginComponentSentinel&&manufacturer==kVst3PluginComponentSentinel;if(!vst3&&(!type||!subtype||!manufacturer))throw Error("Invalid plug-in component identifier");const auto size=static_cast<size_t>(sqlite3_column_bytes(plugins.get(),10));const auto perPluginLimit=vst3?kMaxVst3PluginStateBytes:kMaxAudioUnitPluginStateBytes;if(size>perPluginLimit||stateBytes>kMaxProjectPluginStateBytes-size)throw Error("Plugin state exceeds project limit");stateBytes+=size;const auto data=static_cast<const uint8_t*>(sqlite3_column_blob(plugins.get(),10));std::vector<uint8_t> bytes(size);if(size){if(!data)throw Error("Missing plug-in state");std::copy_n(data,size,bytes.data());}owner->push_back({static_cast<uint64_t>(integer(plugins.get(),3)),type,subtype,manufacturer,string(plugins.get(),7),bypassed!=0,static_cast<uint32_t>(latency),std::move(bytes),{},static_cast<PluginHostingMode>(hostingMode)});}
        if(rc!=SQLITE_DONE)throw Error("Cannot read channel plug-ins");
    } else if(formatVersion>=10){
        auto plugins=prepare(db.get(),"SELECT position,id,type,subtype,manufacturer,name,bypassed,latency_frames,state FROM master_plugins ORDER BY position");size_t stateBytes=0;
        while((rc=sqlite3_step(plugins.get()))==SQLITE_ROW){if(integer(plugins.get(),0)!=static_cast<int64_t>(state.masterInserts.size()))throw Error("Invalid master insert order");const auto bypassed=integer(plugins.get(),6),latency=integer(plugins.get(),7);if(bypassed<0||bypassed>1||latency<0||latency>48000*10||sqlite3_column_type(plugins.get(),8)!=SQLITE_BLOB)throw Error("Invalid master insert state");const auto type=static_cast<uint32_t>(integer(plugins.get(),2)),subtype=static_cast<uint32_t>(integer(plugins.get(),3)),manufacturer=static_cast<uint32_t>(integer(plugins.get(),4));const auto vst3=type==kVst3PluginComponentSentinel&&subtype==kVst3PluginComponentSentinel&&manufacturer==kVst3PluginComponentSentinel;if(!vst3&&(!type||!subtype||!manufacturer))throw Error("Invalid plug-in component identifier");const auto size=static_cast<size_t>(sqlite3_column_bytes(plugins.get(),8));const auto perPluginLimit=vst3?kMaxVst3PluginStateBytes:kMaxAudioUnitPluginStateBytes;if(size>perPluginLimit||stateBytes>kMaxProjectPluginStateBytes-size)throw Error("Plugin state exceeds project limit");stateBytes+=size;const auto data=static_cast<const uint8_t*>(sqlite3_column_blob(plugins.get(),8));std::vector<uint8_t> bytes(size);if(size){if(!data)throw Error("Missing plug-in state");std::copy_n(data,size,bytes.data());}state.masterInserts.push_back({static_cast<uint64_t>(integer(plugins.get(),1)),type,subtype,manufacturer,string(plugins.get(),5),bypassed!=0,static_cast<uint32_t>(latency),std::move(bytes),{}});}
        if(rc!=SQLITE_DONE)throw Error("Cannot read master inserts");
    }
    if(formatVersion>=14){
        auto automation=prepare(db.get(),"SELECT plugin_id,parameter_id,lane_position,point_position,frame,normalized_value,name FROM plugin_parameter_automation ORDER BY plugin_id,lane_position,point_position");size_t totalPoints=0;
        const auto findPlugin=[&](uint64_t pluginID)->PluginInsert*{for(auto& track:state.tracks)for(auto& plugin:track.inserts)if(plugin.id==pluginID)return &plugin;for(auto& bus:state.buses)for(auto& plugin:bus.inserts)if(plugin.id==pluginID)return &plugin;for(auto& plugin:state.masterInserts)if(plugin.id==pluginID)return &plugin;return nullptr;};
        while((rc=sqlite3_step(automation.get()))==SQLITE_ROW){const auto pluginID=static_cast<uint64_t>(integer(automation.get(),0));auto* plugin=findPlugin(pluginID);if(!plugin)throw Error("Plug-in parameter automation owner not found");const auto parameterRaw=integer(automation.get(),1),lanePosition=integer(automation.get(),2),pointPosition=integer(automation.get(),3);if(parameterRaw<0||parameterRaw>static_cast<int64_t>(UINT32_MAX)||lanePosition<0||pointPosition<0||(sqlite3_column_type(automation.get(),5)!=SQLITE_FLOAT&&sqlite3_column_type(automation.get(),5)!=SQLITE_INTEGER))throw Error("Invalid plug-in parameter automation point");if(static_cast<size_t>(lanePosition)>plugin->parameterAutomation.size())throw Error("Invalid plug-in parameter automation lane order");const auto parameterID=static_cast<uint32_t>(parameterRaw);const auto name=string(automation.get(),6);if(static_cast<size_t>(lanePosition)==plugin->parameterAutomation.size()){if(pointPosition!=0)throw Error("Invalid plug-in parameter automation point order");plugin->parameterAutomation.push_back({parameterID,name,{}});}auto& lane=plugin->parameterAutomation[static_cast<size_t>(lanePosition)];if(lane.parameterID!=parameterID||lane.name!=name||pointPosition!=static_cast<int64_t>(lane.points.size()))throw Error("Invalid plug-in parameter automation lane");if(totalPoints>=kMaxProjectPluginParameterAutomationPoints)throw Error("Plug-in parameter automation exceeds project limit");lane.points.push_back({static_cast<uint64_t>(integer(automation.get(),4)),sqlite3_column_double(automation.get(),5)});++totalPoints;}
        if(rc!=SQLITE_DONE)throw Error("Cannot read plug-in parameter automation");
    }
    if(formatVersion>=16){
        auto midiClips=prepare(db.get(),"SELECT track_id,position,start,length,lane FROM midi_clips ORDER BY track_id,position");
        while((rc=sqlite3_step(midiClips.get()))==SQLITE_ROW){const auto trackID=static_cast<uint64_t>(integer(midiClips.get(),0));auto track=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& item){return item.id==trackID;});if(track==state.tracks.end()||integer(midiClips.get(),1)!=static_cast<int64_t>(track->midiClips.size()))throw Error("Invalid draft MIDI clip order");const auto lane=integer(midiClips.get(),4);if(lane<0)throw Error("Invalid MIDI clip lane");track->midiClips.push_back({static_cast<uint64_t>(integer(midiClips.get(),2)),static_cast<uint64_t>(integer(midiClips.get(),3)),{},static_cast<int>(lane)});}
        if(rc!=SQLITE_DONE)throw Error("Cannot read draft MIDI clips");
        auto midiNotes=prepare(db.get(),"SELECT track_id,clip_position,position,start,length,pitch,channel,velocity FROM midi_notes ORDER BY track_id,clip_position,position");size_t totalNotes=0;
        while((rc=sqlite3_step(midiNotes.get()))==SQLITE_ROW){const auto trackID=static_cast<uint64_t>(integer(midiNotes.get(),0));auto track=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& item){return item.id==trackID;});if(track==state.tracks.end())throw Error("MIDI note owner not found");const auto clipPosition=integer(midiNotes.get(),1),notePosition=integer(midiNotes.get(),2);if(clipPosition<0||clipPosition>=static_cast<int64_t>(track->midiClips.size()))throw Error("Invalid draft MIDI note order");auto& clip=track->midiClips[static_cast<size_t>(clipPosition)];if(notePosition!=static_cast<int64_t>(clip.notes.size()))throw Error("Invalid draft MIDI note order");const auto pitch=integer(midiNotes.get(),5),channel=integer(midiNotes.get(),6),velocity=integer(midiNotes.get(),7);if(pitch<0||pitch>127||channel<0||channel>15||velocity<1||velocity>127)throw Error("Invalid MIDI note fields");if(totalNotes>=kMaxMidiNotesPerProject)throw Error("MIDI notes exceed project limit");++totalNotes;clip.notes.push_back({static_cast<uint64_t>(integer(midiNotes.get(),3)),static_cast<uint64_t>(integer(midiNotes.get(),4)),static_cast<uint8_t>(pitch),static_cast<uint8_t>(channel),static_cast<uint8_t>(velocity)});}
        if(rc!=SQLITE_DONE)throw Error("Cannot read draft MIDI notes");
    }
    if(formatVersion>=17){
        // v17 files own their maps: the reader clears the defaults, accepts
        // whatever the tables contain in frame order, and lets validate()
        // reject malformed ranges, sizes or a missing frame-0 anchor.
        state.tempo.clear();
        auto tempo=prepare(db.get(),"SELECT frame,bpm FROM tempo_points ORDER BY frame");
        while((rc=sqlite3_step(tempo.get()))==SQLITE_ROW){
            if(sqlite3_column_type(tempo.get(),1)!=SQLITE_FLOAT&&sqlite3_column_type(tempo.get(),1)!=SQLITE_INTEGER)throw Error("Invalid tempo value");
            state.tempo.push_back({static_cast<uint64_t>(integer(tempo.get(),0)),sqlite3_column_double(tempo.get(),1)});
        }
        if(rc!=SQLITE_DONE)throw Error("Cannot read draft tempo points");
        state.timeSignatures.clear();
        auto timeSignatures=prepare(db.get(),"SELECT frame,numerator,denominator FROM time_signature_points ORDER BY frame");
        while((rc=sqlite3_step(timeSignatures.get()))==SQLITE_ROW){
            const auto numerator=integer(timeSignatures.get(),1),denominator=integer(timeSignatures.get(),2);
            if(numerator<0||numerator>255||denominator<0||denominator>255)throw Error("Invalid time signature fields");
            state.timeSignatures.push_back({static_cast<uint64_t>(integer(timeSignatures.get(),0)),static_cast<uint8_t>(numerator),static_cast<uint8_t>(denominator)});
        }
        if(rc!=SQLITE_DONE)throw Error("Cannot read draft time signature points");
    }
    if(formatVersion>=18){
        // v18 files own the marker lane. Unlike tempo, zero markers is the
        // legal default, so pre-v18 drafts simply keep the empty vector. The
        // frame primary key plus ORDER BY make storage order irrelevant; the
        // reader only bounds the row count here and leaves ranges, name
        // encoding and duplicates (after a dropped primary key) to validate().
        state.markers.clear();
        auto markers=prepare(db.get(),"SELECT frame,name FROM markers ORDER BY frame");
        while((rc=sqlite3_step(markers.get()))==SQLITE_ROW){
            if(state.markers.size()>=kMaxMarkersPerProject)throw Error("Too many markers");
            state.markers.push_back({static_cast<uint64_t>(integer(markers.get(),0)),string(markers.get(),1)});
        }
        if(rc!=SQLITE_DONE)throw Error("Cannot read draft markers");
    }
    validate(state); return state;
}
}
