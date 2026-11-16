# SPDX-License-Identifier: MIT
"""Run Iris sources and clients on a private PipeWireAO daemon."""

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
    bindir, client, plugin, installed_plugins, installed_modules, *ctis = map(Path, sys.argv[1:])
    if len(ctis) == 1:
        devices = {
            "iris-mock": {
                "api.iris.producer": str(ctis[0]),
                "api.iris.acquisition-control": "stream-only",
                "api.iris.buffer-count": 1,
            },
        }
    else:
        edt, andor = ctis
        devices = {
            "iris-edt": {
                "api.iris.producer": str(edt),
                "api.iris.acquisition-control": "stream-only",
                "api.iris.initial-controls": ["TriggerMode", "Off"],
            },
            "iris-andor": {
                "api.iris.producer": str(andor),
                "api.iris.serial-number": "SFT-1000",
                "api.iris.initial-controls": ["AcquisitionMode", "Continuous", "ExposureTime", "2000"],
            },
        }
    objects = [
        {
            "factory": "spa-node-factory",
            "args": {
                "factory.name": "api.iris.source",
                "node.name": name,
                "node.loop.name": name,
                **properties,
            },
        }
        for name, properties in devices.items()
    ]
    with TemporaryDirectory(prefix="pwao-iris-") as directory:
        runtime = Path(directory)
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

        def tool(name, *arguments):
            return subprocess.run(
                [bindir / name, *arguments], env=env, text=True, timeout=2,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )

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
                intervals = []
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
                            name, count, first_pts, last_pts = stdout.split()
                            print(f"{name}: {count} valid frames")
                            intervals.append((int(first_pts), int(last_pts)))
                            poll.unregister(key.fileobj)
                if len(intervals) > 1 and max(start for start, _ in intervals) >= min(end for _, end in intervals):
                    raise RuntimeError("the two graph streams did not overlap")
                for process in processes:
                    stop(process)
                    if process.returncode:
                        raise RuntimeError(f"capture client exited with status {process.returncode}")
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
