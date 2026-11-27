"""Offline resident mailbox/stop checks with the real native wait implementation.

This does not load an audio SDK, contact an account or operate a device. Run on
Linux; --stock-root also repeats shell semantics with the ROM's BusyBox under
qemu. --arm-helper runs that exact production ELF in stock glibc instead of
the default freshly built host ELF from the production account-wait.c.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parent
HELPER = None
SHELL = ["sh"]
REPORT_DIR = None


def eventually(predicate, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.01)
    raise AssertionError("fixture deadline expired")


def process_ended(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] in ("Z", "X")
    except FileNotFoundError:
        return True


class NativeResidentWaitTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="account-resident-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.processes = []
        self.waiters = {}
        self.paths = []
        self.addCleanup(self.cleanup)

    def cleanup(self):
        # Cleanup may follow a deliberately killed parent or a failed assert.
        # Stop only helper PIDs observed directly under this fixture, never an
        # executable-name search or another agent's process.
        for child, birth in self.waiters.items():
            try:
                current = Path(f"/proc/{child}/stat").read_text().rsplit(")", 1)[1].split()
                if current[19] == birth and current[0] not in ("Z", "X"):
                    os.kill(child, signal.SIGTERM)
            except FileNotFoundError:
                pass
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=2)
        for path in self.paths:
            path.unlink(missing_ok=True)

    def fixture(self, helper=None):
        ident = "idle-wait-" + uuid.uuid4().hex
        script = self.root / (ident + ".sh")
        pidfile = self.root / (ident + ".pid")
        output = self.root / (ident + ".dispatch")
        event = Path("/tmp/lx06-wake-" + script.name + ".event")
        lock = Path("/tmp/lx06-" + ident + "-resident.lock")
        self.paths.extend([event, lock])
        functions = (ROOT / "account-voice.sh").read_text().split(
            "# Audio components come from the unified firmware.", 1)[0]
        # Override only the session side effect. The real resident, lock,
        # marker parser, helper startup, signal traps and cleanup stay intact.
        script.write_text(
            functions
            + "\nPROVIDER=" + shlex.quote(ident)
            + "\nLX06_ACCOUNT_WAIT=" + shlex.quote(str(helper or HELPER))
            + "\nvoice_warmup() { :; }\nvoice_session() { printf '%s\\n' \"$1\" >> "
            + shlex.quote(str(output)) + "; }\n"
            + "printf '%s' \"$$\" > " + shlex.quote(str(pidfile))
            + '\nvoice_resident "$@"\n'
        )
        return {"script": script, "pidfile": pidfile, "output": output, "event": event, "lock": lock}

    def start(self, fixture, traced=False, args=()):
        command = [*SHELL, str(fixture["script"]), *args]
        if traced:
            fixture["trace"] = self.root / "process.trace"
            command = ["strace", "-f", "-qq", "-e", "trace=process", "-o", str(fixture["trace"]), *command]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(process)
        fixture["process"] = process
        return process

    def waiter(self, fixture):
        def inspect():
            if not fixture["pidfile"].exists():
                return None
            text = fixture["pidfile"].read_text()
            if not text:
                return None
            parent = int(text)
            try:
                children = Path(f"/proc/{parent}/task/{parent}/children").read_text().split()
                for child in children:
                    try:
                        if b"--wait-event\0" in Path(f"/proc/{child}/cmdline").read_bytes():
                            # Seeing a shell/qemu wrapper's argv is not native
                            # readiness: wait until the real helper registered
                            # its watch (after signals and parent-death setup).
                            fds = Path(f"/proc/{child}/fd")
                            for fd in fds.iterdir():
                                if str(fd.readlink()) == "anon_inode:inotify" and "inotify wd:" in Path(f"/proc/{child}/fdinfo/{fd.name}").read_text():
                                    self.waiters[int(child)] = Path(f"/proc/{child}/stat").read_text().rsplit(")", 1)[1].split()[19]
                                    return int(child)
                    except FileNotFoundError:
                        pass
            except FileNotFoundError:
                pass
            return None
        return eventually(inspect)

    def publish(self, fixture, word="测试唤醒", offset=0, atomic=False, stamp=None):
        if stamp is None:
            # Linux uptime is the same clock used by the production mailbox.
            stamp = float(Path("/proc/uptime").read_text().split()[0]) + offset
        text = f"{word} 0.1 {stamp}\n"
        if atomic:
            temporary = fixture["event"].with_suffix(".router")
            self.paths.append(temporary)
            temporary.write_text(text)
            temporary.replace(fixture["event"])
        else:
            fixture["event"].write_text(text)

    def dispatches(self, fixture):
        return fixture["output"].read_text().splitlines() if fixture["output"].exists() else []

    def stop(self, fixture, signum=signal.SIGTERM):
        parent = int(fixture["pidfile"].read_text())
        waiter = self.waiter(fixture)
        before = time.monotonic()
        os.kill(parent, signum)  # Parent PID only, never the process group.
        out, error = fixture["process"].communicate(timeout=2)
        latency = time.monotonic() - before
        self.assertEqual(fixture["process"].returncode, 0, error)
        self.assertFalse(error, error)
        self.assertLess(latency, .75)
        eventually(lambda: process_ended(waiter))
        self.assertFalse(fixture["event"].exists())
        return out

    def test_native_protocol(self):
        process = subprocess.run([str(HELPER), "--wait-protocol"], capture_output=True, timeout=2)
        self.assertEqual((process.returncode, process.stdout, process.stderr), (0, b"LX06WAIT/1\n", b""))

    def test_fresh_event_dispatches_once(self):
        fixture = self.fixture(); self.start(fixture); self.waiter(fixture)
        self.publish(fixture)
        eventually(lambda: self.dispatches(fixture) == ["测试唤醒"])
        self.waiter(fixture)
        self.assertFalse(fixture["event"].exists())
        self.stop(fixture)

    def test_direct_wakeup_dispatches_before_idle_wait(self):
        fixture = self.fixture(); self.start(fixture, args=("--wakeup", "直接唤醒"))
        self.waiter(fixture)
        self.assertEqual(self.dispatches(fixture), ["直接唤醒"])
        self.stop(fixture)

    def test_startup_discards_preexisting_mailbox(self):
        fixture = self.fixture(); self.publish(fixture)
        self.start(fixture); self.waiter(fixture)
        self.assertEqual(self.dispatches(fixture), [])
        self.assertFalse(fixture["event"].exists())
        self.stop(fixture)

    def test_empty_event_waits_until_recording_is_written(self):
        fixture = self.fixture(); self.start(fixture); child = self.waiter(fixture)
        fixture["event"].touch(); time.sleep(.05)
        self.assertEqual(self.waiter(fixture), child)
        self.assertEqual(self.dispatches(fixture), [])
        self.publish(fixture)
        eventually(lambda: self.dispatches(fixture) == ["测试唤醒"])
        self.stop(fixture)

    def test_atomic_router_rename_dispatches(self):
        fixture = self.fixture(); self.start(fixture); self.waiter(fixture)
        self.publish(fixture, atomic=True)
        eventually(lambda: self.dispatches(fixture) == ["测试唤醒"])
        self.stop(fixture)

    def rejected_stamp(self, **kwargs):
        fixture = self.fixture(); self.start(fixture); previous = self.waiter(fixture)
        self.publish(fixture, **kwargs)
        eventually(lambda: not fixture["event"].exists())
        self.assertNotEqual(self.waiter(fixture), previous)
        self.assertEqual(self.dispatches(fixture), [])
        self.stop(fixture)

    def test_stale_stamp_ignored(self):
        self.rejected_stamp(offset=-10)

    def test_future_stamp_ignored(self):
        self.rejected_stamp(offset=10)

    def test_invalid_stamp_ignored(self):
        self.rejected_stamp(stamp="invalid")

    def test_consecutive_events_keep_resident_and_route(self):
        fixture = self.fixture(); process = self.start(fixture); self.waiter(fixture)
        for number in range(3):
            self.publish(fixture, word=f"唤醒{number}")
            eventually(lambda: len(self.dispatches(fixture)) == number + 1)
            self.waiter(fixture)
        self.assertIsNone(process.poll())
        self.assertEqual(self.dispatches(fixture), ["唤醒0", "唤醒1", "唤醒2"])
        self.stop(fixture)

    def test_two_script_mailboxes_are_independent(self):
        left, right = self.fixture(), self.fixture()
        self.start(left); self.start(right)
        self.waiter(left); other_waiter = self.waiter(right)
        self.publish(left, word="左侧")
        eventually(lambda: self.dispatches(left) == ["左侧"])
        self.assertEqual(self.dispatches(right), [])
        self.assertEqual(self.waiter(right), other_waiter)
        self.publish(right, word="右侧")
        eventually(lambda: self.dispatches(right) == ["右侧"])
        self.stop(left); self.stop(right)

    def test_parent_only_term_reaps_waiter(self):
        fixture = self.fixture(); self.start(fixture); self.waiter(fixture)
        self.stop(fixture, signal.SIGTERM)

    def test_parent_only_int_reaps_waiter(self):
        fixture = self.fixture(); self.start(fixture); self.waiter(fixture)
        self.stop(fixture, signal.SIGINT)

    def test_parent_only_hup_reaps_waiter(self):
        fixture = self.fixture(); self.start(fixture); self.waiter(fixture)
        self.stop(fixture, signal.SIGHUP)

    def test_managed_uninstall_stop_after_script_removal(self):
        fixture = self.fixture(); self.start(fixture); self.waiter(fixture)
        fixture["script"].unlink()
        self.stop(fixture)  # keeper/web stop is the existing deletion owner.
        result = subprocess.run(["flock", "-n", str(fixture["lock"]), "true"], timeout=2)
        self.assertEqual(result.returncode, 0)

    def test_missing_helper_stops_without_polling_or_dispatch(self):
        fixture = self.fixture(helper=self.root / "absent")
        process = self.start(fixture, args=("--wakeup", "直接唤醒"))
        out, error = process.communicate(timeout=2)
        # Startup/wait failures must reach the keeper after resident cleanup.
        self.assertEqual(process.returncode, 1)
        self.assertIn(b"matched firmware idle-wait component missing", error)
        self.assertEqual(out, b"")
        self.assertEqual(self.dispatches(fixture), [])

    def test_wrong_protocol_stops_without_dispatch(self):
        wrong = self.root / "wrong-protocol"
        wrong.write_text("#!/bin/sh\nprintf 'OTHER/1\\n'\n"); wrong.chmod(0o700)
        fixture = self.fixture(helper=wrong); process = self.start(fixture)
        _, error = process.communicate(timeout=2)
        # Startup/wait failures must reach the keeper after resident cleanup.
        self.assertEqual(process.returncode, 1)
        self.assertIn(b"matched firmware idle-wait component missing", error)
        self.assertEqual(self.dispatches(fixture), [])

    def test_native_io_error_exits_without_busy_retry(self):
        fixture = self.fixture(); process = self.start(fixture); self.waiter(fixture)
        destination = self.root / "untrusted-target"; destination.write_text("payload")
        fixture["event"].symlink_to(destination)
        _, error = process.communicate(timeout=2)
        # Startup/wait failures must reach the keeper after resident cleanup.
        self.assertEqual(process.returncode, 1)
        self.assertEqual(error, b"account-wait: idle-wait failed (70); resident stopped\n")
        self.assertEqual(self.dispatches(fixture), [])
        self.assertEqual(destination.read_text(), "payload")

    def test_duplicate_resident_lock_does_not_replace_first(self):
        fixture = self.fixture(); first = self.start(fixture); waiter = self.waiter(fixture)
        second = subprocess.run([*SHELL, str(fixture["script"])], capture_output=True, timeout=2)
        self.assertEqual((second.returncode, second.stdout, second.stderr), (0, b"", b""))
        # Second start overwrites only the fixture PID file, restore first PID.
        fixture["pidfile"].write_text(str(first.pid))
        self.assertIsNone(first.poll())
        self.assertEqual(self.waiter(fixture), waiter)
        self.stop(fixture)

    def test_parent_death_does_not_leave_waiter_or_lock(self):
        fixture = self.fixture(); process = self.start(fixture); child = self.waiter(fixture)
        process.kill(); process.communicate(timeout=2)
        eventually(lambda: process_ended(child))
        result = subprocess.run(["flock", "-n", str(fixture["lock"]), "true"], timeout=2)
        self.assertEqual(result.returncode, 0)

    def test_idle_has_one_wait_launch_and_zero_sleep_execs(self):
        fixture = self.fixture(); self.start(fixture, traced=True); child = self.waiter(fixture)
        time.sleep(.4)
        self.assertEqual(self.waiter(fixture), child)
        self.stop(fixture)
        trace = fixture["trace"].read_text()
        wait_execs = [line for line in trace.splitlines() if 'execve("' + str(HELPER) + '"' in line and '"--wait-event"' in line]
        sleep_execs = [line for line in trace.splitlines() if "execve(" in line and '"sleep"' in line]
        self.assertEqual(len(wait_execs), 1, trace)
        self.assertEqual(sleep_execs, [], trace)


class NativeHelperContractTest(unittest.TestCase):
    """Real helper exit/error behavior; no wrapper may fake success codes."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="account-wait-contract-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def run_helper(self, args, expected):
        process = subprocess.run([str(HELPER), *map(str, args)], capture_output=True, timeout=2)
        self.assertEqual((process.returncode, process.stdout, process.stderr), (expected, b"", b""))

    def test_invalid_argument_stays_64(self):
        self.run_helper(["--wait-event", "relative"], 64)
        self.run_helper(["--wait-children", "1", "0", "2", "0"], 64)

    def test_existing_event(self):
        event = self.root / "existing"; event.write_text("wake")
        self.run_helper(["--wait-event", event], 0)

    def test_symlink_stays_70(self):
        target = self.root / "target"; target.write_text("wake")
        event = self.root / "link"; event.symlink_to(target)
        self.run_helper(["--wait-event", event], 70)

    def test_oversize_stays_70(self):
        event = self.root / "oversize"; event.write_bytes(b"x" * 4097)
        self.run_helper(["--wait-event", event], 70)

    def raw_signal(self, signum):
        process = subprocess.Popen([str(HELPER), "--wait-event", str(self.root / "absent")], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            time.sleep(.08)
            self.assertIsNone(process.poll())
            process.send_signal(signum)
            out, error = process.communicate(timeout=2)
            self.assertEqual((process.returncode, out, error), (130, b"", b""))
        finally:
            if process.poll() is None:
                process.kill(); process.communicate(timeout=2)

    def test_term_stays_130(self):
        self.raw_signal(signal.SIGTERM)

    def test_int_stays_130(self):
        self.raw_signal(signal.SIGINT)

    def test_replaced_directory_uses_same_process_fallback(self):
        directory = self.root / "mailbox"; directory.mkdir()
        event = directory / "event"
        process = subprocess.Popen([str(HELPER), "--wait-event", str(event)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            time.sleep(.08); self.assertIsNone(process.poll())
            directory.rename(self.root / "old-mailbox")
            directory.mkdir(); event.write_text("wake")
            out, error = process.communicate(timeout=2)
            self.assertEqual((process.returncode, out, error), (0, b"", b""))
        finally:
            if process.poll() is None:
                process.kill(); process.communicate(timeout=2)


def main():
    global HELPER, SHELL, REPORT_DIR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--stock-root", type=Path)
    parser.add_argument("--arm-helper", type=Path, help="Run this exact production ARM ELF under stock glibc/qemu")
    args = parser.parse_args()
    REPORT_DIR = args.output.resolve()
    REPORT_DIR.mkdir(parents=True, exist_ok=False)
    runtime_tmp = None
    runtime = None
    if args.stock_root:
        stock = args.stock_root.resolve(strict=True)
        # QEMU -L translates an existing absolute /tmp to STOCK/tmp, while a
        # new event below it may stay on host /tmp. Supply only stock libraries
        # in a temporary prefix, so directory watch and open see the same path.
        # Never delete/modify STOCK/tmp to make a test appear to pass.
        runtime_tmp = tempfile.TemporaryDirectory(prefix="account-wait-stock-libs-")
        runtime = Path(runtime_tmp.name)
        for relative in ["lib", "usr/lib"]:
            destination = runtime / relative; destination.mkdir(parents=True)
            for library in (stock / relative).iterdir():
                if library.is_file():
                    (destination / library.name).symlink_to(library.resolve(strict=True))
    actual_arm = args.arm_helper.resolve(strict=True) if args.arm_helper else None
    if actual_arm:
        if not args.stock_root:
            raise ValueError("--arm-helper requires --stock-root")
        HELPER = REPORT_DIR / "account-wait-arm-run"
        HELPER.write_text("#!/bin/sh\nexec " + " ".join(shlex.quote(str(arg)) for arg in ["/usr/bin/qemu-arm-static", "-L", runtime, actual_arm]) + ' "$@"\n')
        HELPER.chmod(0o700)
    else:
        HELPER = REPORT_DIR / "account-wait-host"
        subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", str(ROOT / "account-wait.c"), "-o", str(HELPER)], check=True)
    report = {"passed": False, "device_access": False, "cloud_requests": 0,
              "network_namespace_isolated": os.readlink("/proc/self/ns/net") != os.readlink("/proc/1/ns/net"),
              "scope": "canonical resident mailbox wait only; no audio state machine",
              "shells": {}, "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in [ROOT / "account-voice.sh", ROOT / "account-wait.c", ROOT / "experiments/account-wait.h", Path(__file__)]}}
    if actual_arm:
        report["actual_stock_arm_helper"] = {"path": str(actual_arm), "bytes": actual_arm.stat().st_size, "sha256": hashlib.sha256(actual_arm.read_bytes()).hexdigest()}
        report["stock_library_only_prefix"] = True
    shells = {"host-sh": ["sh"]}
    if args.stock_root:
        shells["stock-busybox-qemu"] = ["qemu-arm-static", "-L", str(runtime), str(stock / "bin/busybox"), "sh"]
    passed = True
    for label, command in shells.items():
        SHELL = command
        suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(test) for test in [NativeResidentWaitTest, NativeHelperContractTest])
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        report["shells"][label] = {"tests": result.testsRun, "failures": len(result.failures), "errors": len(result.errors), "passed": result.wasSuccessful(), "command": command}
        passed = passed and result.wasSuccessful()
    report["passed"] = passed
    report["helper_sha256"] = hashlib.sha256(HELPER.read_bytes()).hexdigest()
    (REPORT_DIR / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if runtime_tmp:
        runtime_tmp.cleanup()
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
