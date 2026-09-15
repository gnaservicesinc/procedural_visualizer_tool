# Remote connections and depth photos

## Remote settings

**Remote connection details** in the status bar opens the live table in one click,
even when no devices are connected. **Settings → Remotes** shows the same table
first; expand **Manage remote files and network settings** for setup. Settings
apply immediately; Cancel restores the opening settings. Networking remains opt-in.

Export a **Remote file (.pvtremote)** from each extension and import the files in
PVT. Export the **PVT host file (.pvthost)** from PVT and import it in each extension.
Every imported control profile can edit through the normal project validation and
undo path. Display profiles receive output. Removing a saved Remote file revokes
that identity and closes all its connections. Connection pause and controller
selection controls have been removed, including their saved settings and MIDI target.

The address presets are:

- **This computer and devices on the same IP subnets** (default): loopback and
  the actual IP subnets of this computer’s current network interfaces, including
  IPv6. Interface changes are refreshed within three seconds.
- **Any private network**: RFC 1918, IPv6 unique-local `fc00::/7`, IPv6 link-local
  `fe80::/10`, and loopback.
- **Anywhere**: any unicast endpoint. Encrypted signaling relays require this
  preset because they cannot establish the original client’s source address.
- **Only the addresses or ranges below**: individual IPs, CIDR subnets, and
  inclusive start–end ranges separated by spaces or commas.

The live table identifies devices by browser, system, remote type, address and
stable identity. Each browser tab has its own connection; opening another tab
with the same profile does not disconnect an existing tab. Full identity and
extension version are available in the device tooltip. Older extensions’ browser
headers supply descriptive metadata when available. Saved devices retain the last
reported browser/system and endpoint; names are not used as their identity.

Response time is shown in ms, traffic rate in kb/s or Mb/s, and cumulative sent
and received media/transport data in B, KB, MB or GB. Missing measurements show a
dash. Packet loss comes from receiver feedback. These are live measurements,
not an exported support report. Browser information is self-reported and does not
replace public-key authentication.

Connections automatically recover after interruption. Legacy desktop and browser
pause settings are removed on load. Short ICE interruptions get time to recover;
failed connections and repeated unanswered commands trigger automatic retries.

## Portrait and spatial photos

Choose a HEIC, HEIF, JPEG, PNG, or OpenEXR through the normal starting-image picker.
On macOS, ImageIO/AVFoundation extract the color image, available camera depth or
disparity, portrait subject matte, and available hair, skin, teeth, glasses, and sky
mattes. Image orientation is applied to both color and auxiliary images. Missing
mattes are not fabricated. Spatial stereo groups expose their left and right views.
The original-view choice remains available after selecting a cutout.

**Photo images / depth** reopens the inspector. Select a view, mask, or cutout to
preview it, export its full-resolution PNG, use it as the starting image, or remove
it. The inspector's Apply button records depth changes in normal project undo.
Layer copying, removal, project saving, reopening, and undo/redo retain the assets
through the existing attachment registry. Project format 28 / layer format 24 add
these fields; older projects receive disabled, neutral defaults. Binary layout 3
appends the photo fields while retaining readers for layouts 1 and 2.

Camera depth is normalized disparity: white is nearer and black farther. It is
relative height, not a metric-distance export. When only a stereo pair exists,
a bounded block matcher produces a clearly labeled low-resolution draft and a
confidence image, aligned to the left view. Inspect occlusions, flat areas, and
camera alignment before enabling the draft. This is not calibrated 3D reconstruction.

Photo depth is disabled initially. Enable it to adjust strength and up to ten
degrees of shallow tilt per axis without creating a surface. Optional lighting
uses the layer's existing directional-light and environment-map settings, including
EXR maps. Self-shadows use height-field ray marching; environment shadows use its
brightest sampled direction and shared diffuse environment lighting. Hidden surfaces,
large viewpoint changes, and complete 3D cast shadows cannot be reconstructed from
one height field.

With an enabled Plane height map, the mesh adds
`photo_strength * (normalized_photo_depth - 0.5)` to the authored height displacement
at each vertex. The sum is not averaged or clamped to the original map's range.
Normals and bounds include both contributions, and exported Plane OBJ geometry
uses the same composition. Disable either source independently to remove its part.

Native extraction is currently macOS-specific. Other platforms use the installed
Qt image decoder for the primary image; auxiliary extraction is explicitly marked
unavailable there. The resulting PNG assets and saved projects render on all PVT
platforms. Extraction is bounded to 64 megapixels per image and cutout generation
has a 512 MiB retained-image budget; large imports may provide masks without every
full-size cutout. The original files are not modified.

## Validation

Automated coverage includes actual HEIF auxiliary depth/matte and stereo-pair decoding,
color and mask alignment, cutout alpha, CPU/Metal photo-render parity, tiled-depth
alignment, setup and binary codecs, project round trips,
attachment copying/pruning, additive geometry, neutral depth settings, and GUI
import/edit/duplicate/remove/undo/redo. Remote tests cover explicit private ranges,
IPv6, custom ranges, unauthenticated rejection, legacy-pause migration, ICE
peer-reflexive admission, reconnects, simultaneous tabs and real WebRTC media.
