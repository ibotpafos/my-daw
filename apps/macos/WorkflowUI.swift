import AppKit

private struct WorkflowManifest: Decodable {
    struct Action: Decodable { let id:String;let label:String;let uiSlot:String;let operation:String;let inputSchema:String? }
    let id:String;let version:String;let apiVersion:Int;let displayName:String;let capabilities:[String];let actions:[Action]
}

@MainActor
extension DraftApp {
    private func bundledVocalWorkflow() -> WorkflowManifest? {
        let url=Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/Workflows/vocal-preparation/module.json")
        guard let data=try? Data(contentsOf:url),let manifest=try? JSONDecoder().decode(WorkflowManifest.self,from:data),manifest.apiVersion==0,manifest.id=="org.mydaw.vocal-preparation",["project.read","project.edit","audio.read.selection","audio.render.preview"].allSatisfy(manifest.capabilities.contains),manifest.actions.contains(where:{$0.operation=="vocal.prepare"&&$0.uiSlot=="command.palette"&&$0.inputSchema=="vocal-preparation.v1"}) else{return nil}
        return manifest
    }
    private func vocalName(_ bytes:UnsafeRawBufferPointer)->String{String(decoding:bytes.prefix(while:{$0 != 0}),as:UTF8.self)}
    private func valueField(_ value:Double,_ suffix:String)->NSTextField{let field=NSTextField(string:String(format:"%.1f",value));field.alignment = .right;field.placeholderString=suffix;field.widthAnchor.constraint(equalToConstant:72).isActive=true;return field}
    @objc func runVocalWorkflow(){
        guard !isRecording,let manifest=bundledVocalWorkflow()else{storageMessage("Встроенный workflow manifest недоступен или несовместим.");return}
        var snapshot=daw_snapshot();snapshot.struct_size=UInt32(MemoryLayout<daw_snapshot>.size);guard check(daw_get_snapshot(session,&snapshot)),snapshot.track_count>0 else{storageMessage("Добавь аудиодорожки перед запуском workflow.");return}
        var ids:[UInt64]=[];var selectors:[NSButton]=[];let trackList=NSStackView();trackList.orientation = .vertical;trackList.alignment = .leading;trackList.spacing=4
        for index in 0..<snapshot.track_count{var track=daw_track();track.struct_size=UInt32(MemoryLayout<daw_track>.size);guard check(daw_get_track(session,index,&track))else{return};guard track.audio_frames>0 else{continue};let name=withUnsafeBytes(of:track.name,vocalName);let selector=NSButton(checkboxWithTitle:name,target:nil,action:nil);selector.state = .on;selectors.append(selector);ids.append(track.id);trackList.addArrangedSubview(selector)}
        guard !ids.isEmpty else{storageMessage("Workflow требует хотя бы одну дорожку с аудио.");return}
        let baseName=NSTextField(string:"Vocal");baseName.placeholderString="Основа названия";baseName.widthAnchor.constraint(equalToConstant:210).isActive=true
        let targetRms=valueField(-18,"RMS");let peakCeiling=valueField(-6,"Peak");let doubleOffset=valueField(-3,"Double")
        let targets=NSStackView(views:[label("RMS",size:11,color:.secondaryLabelColor),targetRms,label("PEAK",size:11,color:.secondaryLabelColor),peakCeiling,label("DOUBLE",size:11,color:.secondaryLabelColor),doubleOffset]);targets.spacing=6
        let scroll=NSScrollView();scroll.documentView=trackList;scroll.hasVerticalScroller=true;scroll.drawsBackground=false;scroll.widthAnchor.constraint(equalToConstant:430).isActive=true;scroll.heightAnchor.constraint(equalToConstant:min(240,max(70,CGFloat(ids.count)*26))).isActive=true
        let form=NSStackView(views:[baseName,targets,scroll]);form.orientation = .vertical;form.alignment = .leading;form.spacing=10
        let setup=NSAlert();setup.messageText=manifest.displayName;setup.informativeText="Первый выбранный трек станет Lead, остальные — Double. Уровни рассчитываются по pre-fader RMS с peak ceiling.";setup.accessoryView=form;setup.addButton(withTitle:"Анализ и предпросмотр");setup.addButton(withTitle:"Отмена");guard setup.runModal() == .alertFirstButtonReturn else{return}
        let selected=ids.enumerated().compactMap{selectors[$0.offset].state == .on ? $0.element:nil};guard !selected.isEmpty else{storageMessage("Выбери хотя бы одну вокальную дорожку.");return}
        let normalizedBase=String(baseName.stringValue.unicodeScalars.prefix(80));guard let rms=Double(targetRms.stringValue.replacingOccurrences(of:",",with:".")),let peak=Double(peakCeiling.stringValue.replacingOccurrences(of:",",with:".")),let offset=Double(doubleOffset.stringValue.replacingOccurrences(of:",",with:"."))else{storageMessage("Проверь числовые параметры workflow.");return}
        var itemCount:UInt32=0;var afterRevision:UInt64=0;guard check(daw_preview_vocal_preparation(session,selected,UInt32(selected.count),normalizedBase,rms,peak,offset,revision,nil,0,&itemCount,&afterRevision))else{return}
        var items=[daw_vocal_preview](repeating:daw_vocal_preview(),count:Int(itemCount));for index in items.indices{items[index].struct_size=UInt32(MemoryLayout<daw_vocal_preview>.size)};guard check(daw_preview_vocal_preparation(session,selected,UInt32(selected.count),normalizedBase,rms,peak,offset,revision,&items,itemCount,&itemCount,&afterRevision))else{return}
        let lines=items.map{item in let before=withUnsafeBytes(of:item.before_name,vocalName),after=withUnsafeBytes(of:item.after_name,vocalName);let limiter=item.peak_limited != 0 ? " · peak ceiling":"";return "\(before) → \(after)\nRMS \(String(format:"%.1f",item.rms_db)) dBFS · Peak \(String(format:"%.1f",item.peak_db)) dBFS\nФейдер \(String(format:"%+.1f",item.proposed_gain_db)) dB → RMS \(String(format:"%.1f",item.predicted_rms_db)) / Peak \(String(format:"%.1f",item.predicted_peak_db))\(limiter)"}.joined(separator:"\n\n")
        let text=NSTextView(frame:NSRect(x:0,y:0,width:540,height:min(420,max(120,items.count*76))));text.string=lines;text.isEditable=false;text.drawsBackground=false;text.font = .monospacedSystemFont(ofSize:11,weight:.regular)
        let preview=NSAlert();preview.messageText="Предпросмотр · revision \(revision) → \(afterRevision)";preview.informativeText="Изменения будут применены одной Undo-операцией.";preview.accessoryView=text;preview.addButton(withTitle:"Применить");preview.addButton(withTitle:"Отмена");guard preview.runModal() == .alertFirstButtonReturn else{return};_=daw_stop(session);if check(daw_commit_vocal_preparation(session,selected,UInt32(selected.count),normalizedBase,rms,peak,offset,revision)){refresh();pollTransport()}
    }
}
