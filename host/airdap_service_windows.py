"""Native Windows service dispatcher; no pywin32 or wrapper executable needed.

Contract: https://learn.microsoft.com/windows/win32/services/service-entry-point
"""
import ctypes as C
from ctypes import wintypes as W
import logging
import threading


def dispatch(name, run):
    api = C.WinDLL('advapi32', use_last_error=True)
    class Status(C.Structure):
        _fields_ = [(field, W.DWORD) for field in ('kind', 'state', 'accepted', 'win32_exit',
                                                  'service_exit', 'checkpoint', 'wait_hint')]
    Handler = C.WINFUNCTYPE(None, W.DWORD)
    Main = C.WINFUNCTYPE(None, W.DWORD, C.POINTER(W.LPWSTR))
    class Entry(C.Structure):
        _fields_ = [('name', W.LPWSTR), ('main', Main)]
    api.RegisterServiceCtrlHandlerW.argtypes = [W.LPCWSTR, Handler]
    api.RegisterServiceCtrlHandlerW.restype = W.HANDLE
    api.SetServiceStatus.argtypes = [W.HANDLE, C.POINTER(Status)]
    api.SetServiceStatus.restype = W.BOOL
    api.StartServiceCtrlDispatcherW.argtypes = [C.POINTER(Entry)]
    api.StartServiceCtrlDispatcherW.restype = W.BOOL
    stop = threading.Event()
    finished = threading.Event()
    lock = threading.Lock()
    status = Status(0x10, 2, 0, 0, 0, 1, 30000)
    handle = None
    exit_code = 0

    def report(state, error=0):
        with lock:
            status.state = state
            status.accepted = 5 if state == 4 else 0  # STOP | SHUTDOWN only while running.
            status.win32_exit = error
            status.wait_hint = 30000 if state in (2, 3) else 0
            status.checkpoint = status.checkpoint + 1 if state in (2, 3) else 0
            if not api.SetServiceStatus(handle, C.byref(status)):
                raise C.WinError(C.get_last_error())

    @Handler
    def control(code):
        if code in (1, 5):
            stop.set()
            report(3)
        elif code == 4:
            with lock:
                api.SetServiceStatus(handle, C.byref(status))

    @Main
    def service_main(_argc, _argv):
        nonlocal handle, exit_code
        handle = api.RegisterServiceCtrlHandlerW(name, control)
        if not handle:
            exit_code = C.get_last_error()
            return
        report(2)
        def pending():
            # Keep SCM informed while an already-started OTA write drains on stop.
            while not finished.wait(5):
                if stop.is_set():
                    report(3)
        thread = threading.Thread(target=pending, daemon=True)
        thread.start()
        try:
            run(stop, lambda: report(3 if stop.is_set() else 4))
        except Exception as error:
            logging.error('Windows service failed (%s)', type(error).__name__)
            exit_code = 1
        finally:
            finished.set()
            thread.join()
            report(1, exit_code)

    table = (Entry * 2)(Entry(name, service_main), Entry(None, Main()))
    if not api.StartServiceCtrlDispatcherW(table):
        raise C.WinError(C.get_last_error())
    return exit_code
