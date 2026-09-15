# Remote connections and depth photos

## Remote settings

Application Settings → Remotes contains the full pairing manager. The Settings
menu's Networking & Remotes action opens that tab. Apply saves the remote draft;
OK also applies it. Import, removal, pause, and resume take effect immediately.
Networking remains opt-in.

The address presets are:

- **My subnets and this computer** (default): all current interface subnets and
  loopback, including IPv6 interfaces. Interface changes are refreshed within
  three seconds; already authenticated connections remain visible in the tracker.
- **Private networks**: exactly RFC 1918, IPv6 unique-local `fc00::/7`, IPv6
  link-local `fe80::/10`, and loopback. Documentation, carrier-grade NAT, and other
  special ranges are not silently treated as private networks.
- **Any IP**: any unicast endpoint. Encrypted signaling relays require this preset
  because they cannot establish the original client's source address.
- **Custom**: individual IPs, CIDR subnets, and inclusive start–end ranges,
  separated by spaces or commas. IPv4 and IPv6 may be combined. For example:
  `192.168.1.0/24, 10.0.0.20-10.0.0.40, fd12:3456::/48`.

Remote endpoint ports default to 1–65535. This restricts the browser's signaling
source port and media ports; it does not set PVT's automatically selected listener
port. Browsers normally allocate ephemeral ports, so narrow ranges can prevent
connections. Changing address or port policy disconnects existing sessions.
Pairing keys and the selected controller are still required independently of
network admission.

The title bar lists authenticated remote identities and roles. The status-bar
connection button and **Pop out live connection tracker** open the live table.
It shows identity, role and edit access, browser/user agent, extension version,
platform, signaling endpoint, media endpoints, connection state, elapsed time,
round-trip time where available, reported media packet loss, and DTLS traffic.
Connected media endpoints are taken from nominated ICE pairs; while negotiating,
allowed candidates are shown. Traffic is measured in kbit/s and cumulative bytes;
packet loss comes from receiver feedback, and absent measurements show a dash.
Browser details are authenticated but self-reported, not proof of device identity.
Older clients can connect without this new descriptive metadata.

**Disconnect and pause** prevents automatic reconnection across restarts.
**Resume selected device** in the Remotes tab permits it again. Removing a paired
identity revokes its keys. Pausing also denies queued desktop commands immediately.

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
IPv6, custom ranges, ports, unauthenticated rejection, persistent pause, ICE
peer-reflexive admission, and a real WebRTC connection.
