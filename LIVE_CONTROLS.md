# Live Controls: audio routing and video output

Live Controls is a tool panel. Its toolbar/menu action shows or hides it without starting or stopping playback, audio, or video output. Space plays or pauses the project from the editor, Live Controls, or the video output window; text editors retain ordinary spaces.

## One video destination

The editor preview renders at canvas resolution. Start Video Output in Live Controls, or Live Preview Output on the main toolbar, to move video to the output window. The editor then displays an output notice instead of painting another preview. Live Controls has no video monitor.

Output starts at 100% canvas resolution unless you previously selected another quality setting. Window size and opening or closing controls do not select a smaller render resolution. Fixed percentages explicitly reduce resolution; Auto may reduce it after missed deadlines. The readout reports delivered dimensions, measured delivery FPS, and scheduled frame deadlines missed since the current playback/FPS epoch. It includes frames the event loop could not schedule, rather than only requests discarded from the renderer queue.

The output window has native move/resize/maximize controls and an Output menu. F11 toggles full screen; Escape returns to a normal window, and a second Escape stops output. Closing the output window also stops output. Closing Live Controls leaves output running. The main preview resumes when output stops.

## Audio inputs and independent output mixes

1. Open **Live Controls → Audio & Video**. Connected capture and playback devices appear in one routing table. Available devices receive ordinary project audio roles; those additions use project history. Manage Roles adds, renames, or removes roles; removing a role removes its associated control/clock routes, and project Undo restores them. Device bindings and mix selections are machine-local settings.
2. Every available clock audio attachment that is not Data-only appears as a looping file input. This includes layer-clock attachments. These sources reuse the project audio decoder and loop continuously while audio routing runs, independently of video transport.
3. Check **Analysis** on any sources you want mixed into the shared input trim, filters, EQ, gate, calibration, and visual analysis chain. Adjust each source’s Gain %. The existing analysis role identifies this analysis mix for clock and control mappings.
4. Check output-device columns to choose each speaker or headphone mix independently. For example, select a microphone under headphones, and that microphone plus a clock song under speakers. Uncheck a cell to remove that route. Audio outputs receive the source mixes before the analysis-only EQ/gate.
5. Click **Start Audio Routing**. This can run while video is stopped. **Stop Audio Routing** stops those routes. Starting video with existing audio clock/control mappings also starts the same audio routing controls.

Devices are rescanned while controls or routing are active. A single/default input is ready for analysis; speaker routes are opt-in. Missing selected devices remain unavailable rather than silently falling back. Reconnect the device or choose another route. Buffer underrun/overrun counts report audio transport problems. Independent device clocks use bounded buffers and gradual resampling to prevent accumulating drift.

Project-timeline music playback continues when Live Controls opens. The optional looping file source is independent: routing that same file to a speaker while timeline monitoring is also audible will play both. Use the editor’s Audio volume to silence timeline monitoring when using a routed file mix.

## Saved Looks

Saved Looks is the existing Scenes system with clearer naming and instructions. Capture a look, change visual controls, then capture another. During video output, select a saved look and choose Apply to Output. Numeric values can crossfade; discrete switches change at the end. Scene Morph blends two saved looks. Captures are project settings with normal save/load and undo; performing a look does not rewrite authored project values.

## Black first frames

A successfully rendered frame can be black. In the supplied Danger Chimp project, the first frame is opaque black even at full canvas resolution outside the GUI; later frames contain visible pixels. Its Difference composition must be preserved rather than silently modifying the saved artwork.
