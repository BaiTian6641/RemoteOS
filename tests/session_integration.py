#!/usr/bin/env python3
"""A real host process and production C++ client/media core on loopback.

This proves wire integration using explicitly synthetic capture. It does not
claim hardware presentation, virtual displays, login, or device qualification.
"""
import json
import pathlib
import selectors
import subprocess
import sys
import tempfile


def main():
    host, probe = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="remoteos-session-") as temporary:
        root = pathlib.Path(temporary)
        initialized = subprocess.run(
            [host, "--identity", str(root / "host"), "--init"],
            text=True, capture_output=True, check=True, timeout=10,
        )
        identity = json.loads(initialized.stdout)
        config = root / "host.json"
        config.write_text(json.dumps({
            "identity_dir": str(root / "host"),
            "pairing_code_file": identity["pairing_code_file"],
            "bind": "127.0.0.1",
            "port": 0,
            "test_source": True,
            "bandwidth_limit_bps": 1000000000,
            "audio_source": "test",
            "usb_enabled": sys.platform.startswith("linux"),
            "usb_port": 0,
        }))
        with (root / "host.log").open("w+") as log:
            server = subprocess.Popen(
                [host, "--config", str(config)], stdout=subprocess.PIPE,
                stderr=log, text=True,
            )
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(server.stdout, selectors.EVENT_READ)
                    if not selector.select(timeout=10):
                        raise AssertionError("host did not report listening")
                    listening = json.loads(server.stdout.readline())
                if listening.get("fingerprint") != identity["fingerprint"]:
                    raise AssertionError("host identity changed after init")
                result = subprocess.run(
                    [probe, str(listening["port"]), str(root / "client"),
                     identity["fingerprint"], identity["pairing_code_file"]],
                    capture_output=True, text=True, timeout=40,
                )
                print(result.stdout, end="")
                if result.returncode:
                    raise AssertionError(result.stderr)
                if server.poll() is not None:
                    raise AssertionError("host exited during the session")
            except Exception:
                log.flush()
                log.seek(0)
                print("Host diagnostics:\n" + log.read(), file=sys.stderr)
                raise
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                server.stdout.close()


if __name__ == "__main__":
    main()
