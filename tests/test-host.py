# SPDX-License-Identifier: MIT
"""Run two Iris mock sources and clients on a private PipeWireAO daemon."""

import json
import os
import selectors
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
import time


def wait_until(ready, description):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if ready():
            return
        time.sleep(0.02)
    raise RuntimeError(f"timed out waiting for {description}")


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    bindir, client, plugin, installed_plugins, installed_modules, cti = map(Path, sys.argv[1:7])
    control_cti = sys.argv[7] if len(sys.argv) > 7 else None
    generic_pair = len(sys.argv) > 8 and sys.argv[8] == "both"
    paired = {}
    with TemporaryDirectory(prefix="pwao-iris-") as directory:
        runtime = Path(directory)
        # Load the same mock implementation with independent producer state.
        second_cti = runtime / "iris-test-cti-second.cti"
        shutil.copyfile(cti, second_cti)
        devices = {
            "iris-mock-auto": (cti, {}),
            "iris-mock-explicit": (second_cti, {"api.iris.buffer-count": 4}),
        }
        if control_cti:
            paired["iris-mock-auto"] = "PRIMARY-TEST-001" if generic_pair else "CRED2-TEST-001"
            if generic_pair:
                paired["iris-mock-explicit"] = "ALT-TEST-001"
            for name, serial in paired.items():
                devices[name][1].update({
                    "api.iris.serial-number": "test-serial",
                    "api.iris.control-producer": control_cti,
                    "api.iris.camera-serial": serial,
                    "api.iris.runtime-controls": "[ DeviceSerialNumber AcquisitionFrameRate TriggerMode ]",
                })
                if serial == "ALT-TEST-001":
                    devices[name][1].update({
                        "api.iris.control-interface-id": "CameraLinkSerial",
                        "api.iris.control-device-id": "CLSerial-1",
                    })
        objects = [
            {
                "factory": "spa-node-factory",
                "args": {
                    "factory.name": "api.iris.source",
                    "node.name": name,
                    "node.loop.name": name,
                    "api.iris.producer": str(path),
                    "api.iris.acquisition-control": "stream-only",
                    **properties,
                },
            }
            for name, (path, properties) in devices.items()
        ]
        config = runtime / "config"
        dropins = config / "pipewire.conf.d"
        dropins.mkdir(parents=True)
        installed_config = Path(os.environ.get(
            "PIPEWIREAO_CONFIG_DIR", bindir.parent / "share" / "pipewire-ao"
        ))
        for name in ("pipewire.conf", "client.conf"):
            shutil.copyfile(installed_config / name, config / name)
        loops = [{"loop.name": "ordinary", "loop.class": "data.rt", "loop.idle": "eventfd"}]
        loops.extend(
            {"loop.name": name, "loop.class": "data.rt", "loop.idle": "busy-spin"}
            for name in devices
        )
        (dropins / "90-iris.conf").write_text(
            "context.spa-libs = { api.iris.* = iris/libspa-iris }\n"
            f"context.properties = {json.dumps({'context.data-loops': loops})}\n"
            f"context.objects = {json.dumps(objects, indent=4)}\n",
            encoding="utf-8",
        )
        env = os.environ | {
            "PIPEWIREAO_RUNTIME_DIR": directory,
            "PIPEWIREAO_MODULE_DIR": str(installed_modules),
            "PIPEWIREAO_CONFIG_DIR": str(config),
            "PIPEWIREAO_REMOTE": "pipewire-ao-0",
            "PIPEWIREAO_LOG_SYSTEMD": "false",
            "PIPEWIREAO_SPA_PLUGIN_DIR": f"{plugin.parents[1]}:{installed_plugins}",
        }

        if env.get("IRIS_SPA_TEST_HELD_BUFFER"):
            env["IRIS_MOCK_VARY_PIXELS"] = "1"

        if control_cti:
            env.pop("CLP_XML_PATH", None)
            env.pop("CRED2_SIM_HARDWARE_ID", None)
            env.pop("CRED2_SIM_NO_CAMERA", None)
            env["CRED2_SIM_CROP_COLUMNS"] = "0 1"

        def tool(name, *arguments):
            return subprocess.run(
                [bindir / name, *arguments], env=env, text=True, timeout=2,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )

        def check_live_controls(name, serial):
            snapshot = json.loads(tool("pwao-dump").stdout)
            node, = [
                obj for obj in snapshot
                if obj["type"].endswith(":Node")
                and obj.get("info", {}).get("props", {}).get("node.name") == name
            ]
            if node["info"]["state"] != "running":
                raise RuntimeError("paired source is not running")
            node_id = str(node["id"])
            before = tool("pwao-cli", "enum-params", node_id, "Props")
            if before.returncode or any(value not in before.stdout for value in (
                f'"{serial}"', 'Double 120.000000', '"Off"'
            )):
                raise RuntimeError(before.stdout)
            for payload in ('{ params = [ "genicam.AcquisitionFrameRate" 200.0 ] }',
                            '{ params = [ "genicam.TriggerMode" "On" ] }'):
                result = tool("pwao-cli", "set-param", node_id, "Props", payload)
                if result.returncode:
                    raise RuntimeError(result.stdout)
            after = tool("pwao-cli", "enum-params", node_id, "Props")
            if after.returncode or 'Double 200.000000' not in after.stdout or '"On"' not in after.stdout:
                raise RuntimeError(after.stdout)
            print(f"Verified feature writes and readback for {serial} on the running PipeWireAO source")

        processes = []
        with (runtime / "daemon.log").open("w+") as log:
            daemon = subprocess.Popen([bindir / "pipewire-ao"], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_until((runtime / "pipewire-ao-0").is_socket, "private daemon")
                links = [(name, f"{name}-client") for name in devices]
                for _, sink in links:
                    processes.append(subprocess.Popen(
                        [client, sink], env=env, text=True, stdout=subprocess.PIPE,
                    ))
                for source, sink in links:
                    wait_until(lambda: sink in tool("pwao-link", "-i").stdout,
                               f"{sink} input port")
                    result = tool("pwao-link", "-w", source, sink)
                    if result.returncode:
                        raise RuntimeError(result.stdout)
                deadline = time.monotonic() + 7
                with selectors.DefaultSelector() as poll:
                    for process in processes:
                        poll.register(process.stdout, selectors.EVENT_READ, process)
                    while poll.get_map():
                        ready = poll.select(max(0, deadline - time.monotonic()))
                        if not ready:
                            raise RuntimeError("timed out waiting for frame delivery")
                        for key, _ in ready:
                            stdout = key.fileobj.readline()
                            if not stdout:
                                raise RuntimeError(f"capture client exited: {key.data.args}")
                            poll.unregister(key.fileobj)
                for name, serial in paired.items():
                    check_live_controls(name, serial)
                intervals = []
                for process in processes:
                    stop(process)
                    if process.returncode:
                        raise RuntimeError(f"capture client exited with status {process.returncode}")
                    name, count, first_pts, last_pts = process.stdout.readline().split()
                    print(f"{name}: {count} valid frames")
                    intervals.append((int(first_pts), int(last_pts)))
                if max(start for start, _ in intervals) >= min(end for _, end in intervals):
                    raise RuntimeError("the two graph streams did not overlap")
                if daemon.poll() is not None:
                    raise RuntimeError("PipeWireAO daemon exited during capture")
                stop(daemon)
                log.seek(0)
                if "error unset format" in log.read():
                    raise RuntimeError("PipeWireAO could not release the output format")
            except (RuntimeError, ValueError, subprocess.TimeoutExpired):
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                for process in processes:
                    stop(process)
                stop(daemon)


if __name__ == "__main__":
    main()
