# Iris GenTL source for PipeWireAO

`api.iris.source` is a PipeWireAO SPA video source. Each node opens one device
and one GenTL data stream through Iris GenTL. Create multiple nodes to use
multiple devices or producers in one daemon.

## Install

Iris SPA v0.1.0 is qualified with [PipeWireAO 4.0.5](https://github.com/DarrylGamroth/PipeWireAO/releases/tag/v4.0.5)
and [Iris GenTL 0.2.4](https://github.com/DarrylGamroth/iris-gentl/releases/tag/v0.2.4).
Use PipeWireAO 4.0.5 for its dequeued video buffer lifetime fix. The Iris GenTL
packages include the GenApi runtime. Install the GenTL producer and any runtime
libraries it needs separately.

### Install the release on Debian 13 amd64

Download the Iris GenTL Debian 13 package, the PipeWireAO 4.0.5
[Linux x86-64 tarball](https://github.com/DarrylGamroth/PipeWireAO_jll.jl/releases/tag/PipeWireAO-v4.0.5%2B0),
and `iris-spa-0.1.0-debian13-amd64.tar.gz` and `SHA256SUMS` from the
[Iris SPA release](https://github.com/DarrylGamroth/pipewireao-spa-plugin-iris/releases/tag/v0.1.0).
From the download directory, install Iris GenTL and extract the two tarballs
into the same directory.

```sh
sha256sum --check SHA256SUMS
sudo apt install ./iris-gentl_0.2.4_debian13_amd64.deb
pipewireao="$HOME/opt/pipewireao-4.0.5"
mkdir -p "$pipewireao"
tar -xzf PipeWireAO.v4.0.5.x86_64-linux-gnu.tar.gz -C "$pipewireao"
tar -xzf iris-spa-0.1.0-debian13-amd64.tar.gz -C "$pipewireao"
```

For a daemon started from the extracted PipeWireAO distribution, set these
paths in the daemon environment and use the same environment for its CLI tools.

```sh
export PATH="$pipewireao/bin:$PATH"
export LD_LIBRARY_PATH="$pipewireao/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PIPEWIREAO_MODULE_DIR="$pipewireao/lib/pipewire-ao-0.3"
export PIPEWIREAO_SPA_PLUGIN_DIR="$pipewireao/lib/spa-ao-0.2"
export PIPEWIREAO_CONFIG_DIR="$pipewireao/share/pipewire-ao"
```

Put the edited source configuration in `pipewire.conf.d` under the active
configuration directory and restart the daemon. The installed
`lib/spa-ao-0.2/iris/libspa-iris.so` provides the `api.iris.source` factory.

### Build and install from source

Install Meson 1.3 or newer, Ninja, a C compiler, and pkg-config. Use the
PipeWireAO development files and the Iris GenTL headers and shared library.
Set `pipewireao` to the PipeWireAO installation directory. From the v0.1.0
source tree, with the amd64 Iris GenTL `.deb` installed, run

```sh
export PKG_CONFIG_PATH="$pipewireao/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
meson setup build -Diris-prefix=/usr -Diris-libdir=/usr/lib/x86_64-linux-gnu
meson compile -C build
meson test -C build --print-errorlogs
meson install -C build
```

For a relocatable Iris GenTL installation, set `-Diris-prefix` to the extracted
directory and omit `-Diris-libdir` if the library is in its `lib` directory.
Make that library directory visible to the daemon's dynamic loader. Meson
installs `iris/libspa-iris.so` under the directory reported by
`pkg-config --variable=plugindir libspa-ao-0.2`. To try the plugin from the
build tree, prepend `$PWD/build/spa/plugins` to `PIPEWIREAO_SPA_PLUGIN_DIR`.

## Configure a source

Use [examples/90-iris.conf](examples/90-iris.conf) as a daemon configuration
drop-in. Assign each source a unique `node.name` and a named polling data loop
with `loop.idle = busy-spin`. Use a separate polling loop for each source and reserve a CPU per loop in a
production deployment.

| Property | Value |
| --- | --- |
| `api.iris.producer` | Required. Absolute CTI path or filename resolved through `GENICAM_GENTL64_PATH`. |
| `api.iris.serial-number` | Serial number reported by the producer. An interface ID can narrow the search. |
| `api.iris.interface-id`, `api.iris.device-id` | Exact GenTL identity. Use both together to select by ID. |
| `api.iris.stream-index` | Zero-based data stream index. Defaults to `0`. |
| `api.iris.buffer-count` | Exact GenTL acquisition buffer count. Omit to let Iris use its default, which is at least the producer minimum. |
| `api.iris.acquisition-control` | `remote-and-stream` by default. Set `stream-only` when the application or producer controls remote acquisition separately. |
| `api.iris.initial-controls` | Alternating remote GenApi feature names and textual values. Iris applies them in order before reading the image format. |

With no device selector, the producer must discover exactly one camera. Initial
controls support selector-dependent settings by listing the selector first.
Earlier writes stay applied if a later write fails. Set geometry and pixel
format here, then recreate the node to change them.

If an explicit buffer count is below the producer minimum, starting the node
returns `-ERANGE` and logs both the requested count and the producer minimum.
Set a valid count in the configuration and recreate the node to start
acquisition.

The output is raw video. PFNC `Mono8` maps to SPA `GRAY8`. Unpacked
`Mono10`, `Mono12`, `Mono14`, and `Mono16` map to `GRAY16_LE` with the
original sample values. The camera determines the frame cadence. The source
copies each valid image into one mapped SPA buffer and removes row padding.
Iris releases the GenTL buffer before the graph can observe the image. When
negotiated, SPA Header metadata contains the host monotonic reception timestamp
and the node's completion sequence. The timestamp describes reception and
does not establish exposure timing.

The plugin creates no worker threads and does no explicit heap allocation in
its process callback. The callback polls for at most one completion with a
zero timeout, then copies and releases it. The CTI's poll and requeue
implementation still determines the producer-side cost.

## Pair a CLProtocol control CTI with a Grablink source

In the source's `args` block, select the Grablink interface and device
reported by `iris-gentl info`. Supply the physical C-RED2 serial advertised
by the control CTI separately.

```ini
api.iris.producer = "/opt/euresys/egrabber/lib/x86_64/grablink.cti"
api.iris.interface-id = "<Grablink interface ID>"
api.iris.device-id = "<Grablink device ID>"
api.iris.acquisition-control = "stream-only"

api.iris.control-producer = "/path/to/clprotocol-gentl.cti"
api.iris.camera-serial = "<physical C-RED2 serial>"
# Select the control endpoint by exact IDs if discovery cannot match the physical serial.
# api.iris.control-interface-id = "CameraLinkSerial"
# api.iris.control-device-id = "CLSerial-0"
api.iris.runtime-controls = [ "DeviceSerialNumber" "AcquisitionFrameRate" "ExposureTime" ]
```

Set `CLPROTOCOL_GENTL_SERIAL` and `CLPROTOCOL_GENTL_CONFIG` in the daemon
environment as described by the standalone producer. The configuration
pairs that serial with the specified capture endpoint. Check that the
camera is cabled to that endpoint. When both control IDs are set, the
node opens that exact control endpoint. Otherwise it selects by the
serial reported by GenTL discovery. In either mode the plugin verifies
the control CTI's remote `DeviceSerialNumber` and checks that the camera
and receiver agree on
`Width`, `Height`, and `PixelFormat` before it opens the stream. Configure
the camera and receiver geometry before creating the node. The capture
endpoint's `api.iris.initial-controls` can configure its remote node map.

The optional `api.iris.runtime-controls` list exposes named scalar features
on the control CTI through `SPA_PARAM_PropInfo` and `SPA_PARAM_Props`.
Values appear under `genicam.<FeatureName>`. Write one value at a time.
GenApi enumerations use their symbolic strings. Select features that are
safe to change during acquisition. The source holds the control-side
`TLParamsLocked` while it owns the camera when the feature supports writes.
Serial I/O runs on the control thread, independently of polling.

```sh
pwao-cli enum-params "$node_id" Props
pwao-cli set-param "$node_id" Props '{ params = [ "genicam.AcquisitionFrameRate" 200.0 ] }'
pwao-cli enum-params "$node_id" Props
```

## Simulator tests

Build `iris-test-cti.cti` from the Iris source revision pinned in the
[CI workflow](.github/workflows/ci.yml) with `BUILD_TESTING=ON`. The fixture
checks the announced count for the automatic default, a producer minimum
above that default, and an explicit count. The tests also exercise frame
delivery and restart through the SPA node. With the PipeWireAO executable
directory configured, two independently loaded mock CTIs deliver overlapping
streams with automatic and explicit counts on an isolated daemon.

```sh
meson configure build \
  -Dtest-mock-cti=/path/to/iris/build/iris-test-cti.cti \
  -Dtest-pipewireao-bindir="$pipewireao/bin" \
  -Dtest-held-buffer=true
meson test -C build --print-errorlogs
IRIS_MOCK_STACKED_INFO=1 meson test -C build --print-errorlogs
```

GitHub CI runs the factory, acquisition restart, and daemon graph checks on
Debian 13 with Iris GenTL 0.2.4 and PipeWireAO 4.0.5. The held-buffer check
keeps a dequeued buffer through 16 subsequent frames and verifies that its
pixels and metadata stay unchanged. The two commands exercise scalar and
stacked GenTL buffer queries.

The daemon test gets its base configuration from the install tree or from
`PIPEWIREAO_CONFIG_DIR` when set.

To run the paired regression, build the standalone CLProtocol GenTL
producer with its simulator tests enabled. Use its CTI, mock CLSerial
library, and generated port assignments. The Iris in-tree CTI supplies
the simulated capture endpoints.

```sh
meson configure build \
  -Dtest-control-cti=/path/to/clprotocol-gentl.cti \
  -Dtest-clserial=/path/to/tests/libmock-clserial.so \
  -Dtest-clprotocol-gentl-config=/path/to/tests/ports.tsv
meson test -C build --print-errorlogs
```

The paired tests verify frame delivery during blocked serial reads and
writes with the C-RED2 driver and the second mock driver, whose CLProtocol
DeviceID omits the serial token. They reject mismatched hardware serials
and image layouts. With `test-pipewireao-bindir` set, a private PipeWireAO
daemon runs both paired sources and exercises live writes and readback. Physical Grablink capture with the
serial channel open still needs hardware qualification.
