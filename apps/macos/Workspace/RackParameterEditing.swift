import AppKit

extension DraftApp {
    func wireRackParameters() {
        channelRack.onReadParameters = { [weak self] request in
            self?.readRackParameters(request) ?? .unavailable("Проект закрыт.")
        }
        channelRack.parameterPanel.onCommit = { [weak self] request, id, value in
            self?.commitRackParameter(request, id: id, value: value) ?? false
        }
    }
    private func acceptsRackParameterRequest(_ request: RackParameterRequest) -> Bool {
        guard !isRecording, !midiTakeArmed, automationGesture == nil, pluginParameterGesture == nil,
              let session, request == channelRack.parameterPanel.request,
              request.target == channelRack.target, request.pluginID == channelRack.parameterPluginID,
              channelRack.devices.contains(where: { $0.id == request.pluginID && $0.available && $0.canEdit }) else { return false }
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        return daw_get_snapshot(session, &snapshot) == 0 && snapshot.revision == request.revision && revision == request.revision
    }
    private func rackParameterError() -> String {
        var bytes = [CChar](repeating: 0, count: 512)
        daw_error(session, &bytes, bytes.count)
        return String(decoding: bytes.prefix(while: { $0 != 0 }).map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }
    func readRackParameters(_ request: RackParameterRequest) -> RackParameterRead {
        guard acceptsRackParameterRequest(request) else { return .unavailable("Параметры заблокированы или канал изменился.") }
        let target = request.target
        var count: UInt32 = 0
        guard daw_get_insert_parameter_count(session, target.owner, target.ownerID, request.pluginID, &count) == 0 else {
            return .unavailable(rackParameterError())
        }
        guard count <= 65536, request.offset <= count else { return .unavailable("Набор параметров изменился. Вернитесь на предыдущую страницу.") }
        var parameters: [RackParameter] = []
        for index in request.offset..<min(count, request.offset + RackParameterPanelView.pageSize) {
            var raw = daw_au_parameter(); raw.struct_size = UInt32(MemoryLayout<daw_au_parameter>.size)
            guard daw_get_insert_parameter(session, target.owner, target.ownerID, request.pluginID, index, &raw) == 0 else {
                return .unavailable(rackParameterError())
            }
            let name = withUnsafeBytes(of: raw.name) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
            guard let parameter = RackParameter(id: raw.id, name: name, minimum: Double(raw.minimum), maximum: Double(raw.maximum),
                value: Double(raw.value), writable: raw.writable != 0, logarithmic: raw.logarithmic != 0) else {
                return .unavailable("Плагин сообщил некорректный диапазон параметра: \(name)")
            }
            parameters.append(parameter)
        }
        return .loaded(RackParameterPage(total: count, parameters: parameters))
    }
    @discardableResult
    func commitRackParameter(_ request: RackParameterRequest, id: UInt32, value: Double) -> Bool {
        guard acceptsRackParameterRequest(request),
              let parameter = channelRack.parameterPanel.page?.parameters.first(where: { $0.id == id }),
              parameter.accepts(value) else { return false }
        // Do not manufacture Undo entries or stop playback for an unchanged value.
        if Float(value) == Float(parameter.value) { return true }
        let target = request.target
        let result = daw_set_insert_parameter(session, target.owner, target.ownerID, request.pluginID, id, Float(value), request.revision)
        guard result == 0 else {
            let reason = rackParameterError()
            setProjectMessage(reason); status.stringValue = reason
            return false
        }
        // This existing command replaces saved plug-in state and stops playback.
        // Re-read the visible page, including any values the plug-in quantized or
        // coupled to this parameter. Never display the requested value as proof.
        refresh(); pollTransport()
        return true
    }
}
