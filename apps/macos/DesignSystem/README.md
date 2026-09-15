# My DAW UI asset kit

This directory is a standalone, AppKit-consumable visual contract derived from the approved dark DAW reference: low-glare blue-black surfaces, lavender primary actions, mint audio activity and coral recording/destructive emphasis.

Use semantic tokens (`DAWDesignTokens.Color.accent`), not literal values. Use `DAWIcon` for all standard actions; it maps each named intent to a scalable SF Symbol and a Russian accessibility label. Treat the SVG files in `Assets/` as documentation/fallback artwork for places where SF Symbols are unavailable.

`DAWDataVisuals` accepts waveform samples, meter values and MIDI notes supplied by the engine/UI model. It deliberately contains no synthetic audio data, timers or analysis. Controls use `DAWControlState` for normal, hover, selected, disabled, recording, focused and error treatment.

The resource manifest is `ui-kit-manifest.json`; open `docs/assets/ui-kit/catalog.html` directly in a browser to inspect every token and semantic asset without a build step.
