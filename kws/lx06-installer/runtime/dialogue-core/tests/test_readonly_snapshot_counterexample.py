#!/usr/bin/env python3
"""Offline counterexample, not an implementation or production fallback.

Run only through run_readonly_snapshot_counterexample.sh, which creates private
mount/network namespaces and a disposable tmpfs. No account data is involved.
"""
import errno
import fcntl
import os
import subprocess
import sys


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_exact(fd):
    return os.pread(fd, 64, 0)


def reader(fd):
    check((fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE) == os.O_RDONLY,
          'inherited descriptor is not read-only')
    check(read_exact(fd) == b'fixture', 'initial snapshot differs')
    try:
        os.pwrite(fd, b'X', 0)
        raise RuntimeError('reader unexpectedly writable')
    except OSError as e:
        check(e.errno == errno.EBADF, 'unexpected reader write error')
    print('READY', flush=True)
    check(sys.stdin.readline() == 'mutate\n', 'parent synchronization failed')
    check(read_exact(fd) == b'MUTATED!', 'inherited reader did not see changed bytes')
    print('MUTATION_VISIBLE', flush=True)


def sealed_control():
    fd = os.memfd_create('synthetic-control', os.MFD_ALLOW_SEALING)
    try:
        check(os.write(fd, b'fixture') == 7, 'short sealed-control write')
        seals = (fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW |
                 fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, seals)
        check(fcntl.fcntl(fd, fcntl.F_GET_SEALS) & seals == seals,
              'seals missing from reference')
        pid = os.fork()
        if pid == 0:
            try:
                for operation in (lambda: os.pwrite(fd, b'X', 0),
                                  lambda: os.ftruncate(fd, 0),
                                  lambda: os.ftruncate(fd, 8)):
                    try:
                        operation()
                        os._exit(2)
                    except OSError as e:
                        if e.errno != errno.EPERM:
                            os._exit(3)
                os._exit(0)
            except BaseException:
                os._exit(4)
        _, status = os.waitpid(pid, 0)
        check(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
              'reference sealed memfd did not deny writes/shrink/growth')
        check(read_exact(fd) == b'fixture', 'sealed control bytes changed')
        print('sealed control: retained writer denied write/shrink/growth', flush=True)
    finally:
        os.close(fd)


def main():
    rw, ro = sys.argv[1:]
    sealed_control()
    path = os.path.join(rw, 'snapshot')
    writer = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    child = None
    try:
        check(os.write(writer, b'fixture') == 7, 'short fixture write')
        os.fchmod(writer, 0o444)
        reader_fd = os.open(os.path.join(ro, 'snapshot'), os.O_RDONLY)
        try:
            try:
                os.open(os.path.join(ro, 'snapshot'), os.O_WRONLY)
                raise RuntimeError('read-only mount accepted writable open')
            except OSError as e:
                check(e.errno == errno.EROFS, 'mount not proven read-only')
            try:
                fcntl.fcntl(writer, fcntl.F_ADD_SEALS,
                            fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW |
                            fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
                raise RuntimeError('ordinary tmpfs unexpectedly accepted seals; re-evaluate')
            except OSError as e:
                print('ordinary inode F_ADD_SEALS rejected errno=%d' % e.errno, flush=True)
            child = subprocess.Popen([sys.executable, __file__, '--reader', str(reader_fd)],
                                     pass_fds=(reader_fd,), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
            check(child.stdout.readline().strip() == 'READY', 'reader did not become ready')
            # This is a distinct process with an already-open writable OFD from
            # before chmod. The read-only bind view does not revoke this OFD.
            writer_child = os.fork()
            if writer_child == 0:
                try:
                    check(os.pwrite(writer, b'MUTATED!', 0) == 8, 'short write')
                    os._exit(0)
                except BaseException:
                    os._exit(1)
            _, status = os.waitpid(writer_child, 0)
            check(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
                  'independent retained writer failed')
            child.stdin.write('mutate\n')
            child.stdin.flush()
            check(child.stdout.readline().strip() == 'MUTATION_VISIBLE',
                  'reader did not observe mutation')
            check(child.wait(timeout=5) == 0, 'reader child failed')
            print('PASS: read-only mount + chmod + inherited O_RDONLY fd are not a seal')
        finally:
            os.close(reader_fd)
    finally:
        if child is not None and child.poll() is None:
            child.kill()
            child.wait()
        os.close(writer)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--reader':
        reader(int(sys.argv[2]))
    else:
        main()
