# Iris GenTL source for PipeWireAO

`api.iris.source` is a PipeWireAO SPA video source. Each node opens one device
and one GenTL data stream through Iris GenTL. Create multiple nodes to use
multiple devices or producers in one daemon.

## Build and install

Install PipeWireAO with its development files and install
[Iris GenTL v0.2.3](https://github.com/DarrylGamroth/iris-gentl/releases/tag/v0.2.3).
The Iris packages include the GenApi runtime. The plugin links to the
installed Iris C library. Install your camera producer and its runtime
separately.

```sh
export PKG_CONFIG_PATH=/opt/pipewireao/current/lib/pkgconfig
meson setup build -Diris-prefix=/path/to/iris/install
meson compile -C build
meson test -C build --print-errorlogs
meson install -C build
```

For the Ubuntu or Debian `.deb`, configure with `-Diris-prefix=/usr` and
`-Diris-libdir=/usr/lib/x86_64-linux-gnu`. For the relocatable tarball, point
`-Diris-prefix` at the extracted directory. Meson installs the plugin as `iris/libspa-iris.so` below the
SPA plugin directory reported by PipeWireAO. Make Iris, GenApi, and producer
runtime libraries visible to the daemon's dynamic loader. When running from the
build tree, prepend `$PWD/build/spa/plugins` to `PIPEWIREAO_SPA_PLUGIN_DIR`
and include the installed PipeWireAO SPA directory in the same variable.

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
| `api.iris.buffer-count` | GenTL acquisition buffer count. Omit to let Iris choose. |
| `api.iris.acquisition-control` | `remote-and-stream` by default. Set `stream-only` if remote commands are controlled separately, or for a free-running EDT mock. |
| `api.iris.initial-controls` | Alternating remote GenApi feature names and textual values. Iris applies them in order before reading the image format. |

With no device selector, the producer must discover exactly one camera. Initial
controls support selector-dependent settings by listing the selector first.
Earlier writes stay applied if a later write fails. Set geometry and pixel
format here, then recreate the node to change them. This version exposes
startup controls through configuration.

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

## Simulator tests

Build `iris-test-cti.cti` from the v0.2.3 Iris source with `BUILD_TESTING=ON`.
The in-tree test exercises frame delivery and restart through the SPA node.
With the PipeWireAO executable directory configured, it also verifies that
frames reach a client on an isolated daemon.

```sh
meson configure build \
  -Dtest-mock-cti=/path/to/iris/build/iris-test-cti.cti \
  -Dtest-pipewireao-bindir=/opt/pipewireao/current/bin
meson test -C build --print-errorlogs
```

GitHub CI runs the factory, acquisition restart, and daemon graph checks on
Debian 13 with the Iris v0.2.3 package and PipeWireAO v4.0.4.

### Optional external simulators

The factory test runs with only the Iris installation. Supply both CTI paths to
test simultaneous EDT mock and Andor SimCam capture through the SPA API.
Supply the PipeWireAO executable directory to also run the two sources on an
isolated daemon and verify overlapping frame delivery to two graph clients.

```sh
meson configure build \
  -Dtest-edt-cti=/path/to/edtpdv.cti \
  -Dtest-andor-cti=/path/to/AndorSDK3GenTL.cti \
  -Dtest-pipewireao-bindir=/opt/pipewireao/current/bin
export EDTPDV_CLPROTOCOL=/path/to/libCLProtocol_test.so
export LD_LIBRARY_PATH=/path/to/iris/lib:/path/to/genapi/bin/Linux64_x64:/path/to/simcam/lib
meson test -C build --print-errorlogs
```

Use the SimCam runtime that exposes serial `SFT-1000`. Tests select the EDT mock
with 16-bit pixels and disable its native-ring mode for the test process.
The daemon test gets its base configuration from the install tree, or from
`PIPEWIREAO_CONFIG_DIR` when set.

Repeated simulator runs exposed an intermittent invalid free inside the Andor
SimCam runtime. AddressSanitizer traced the free to
`TFifoDataReader_WaitUser::Pulse` in `libatdevsimcam.so`.
