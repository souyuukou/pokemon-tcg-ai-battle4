"""Hard resource limits and root-only parallel aggregation."""

from __future__ import annotations
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from fractions import Fraction
import os
import time
from typing import Callable, Sequence, Any

from .solver import SearchResult


@dataclass(frozen=True, slots=True)
class ResourceLimits:
    workers: int = 2
    rss_limit_bytes: int = 2_700_000_000
    match_seconds: float = 600.0
    reserve_seconds: float = 30.0
    tt_entries_per_worker: int = 250_000
    key_bytes_per_worker: int = 700 * 1024 * 1024


class MatchBudget:
    def __init__(self, limits: ResourceLimits = ResourceLimits()):
        self.limits = limits
        self.started = time.monotonic()
        self.used_seconds = 0.0

    @property
    def remaining(self) -> float:
        return max(0.0, self.limits.match_seconds - self.used_seconds)

    def charge(self, elapsed_seconds: float) -> None:
        """Charge only this player's actual decision time."""
        self.used_seconds += max(0.0, float(elapsed_seconds))

    def deadline(self, requested_seconds: float | None = None) -> float:
        usable = max(0.0, self.remaining - self.limits.reserve_seconds)
        if requested_seconds is not None: usable = min(usable, requested_seconds)
        return time.monotonic() + usable

    def can_expand(self) -> bool:
        return self.remaining > self.limits.reserve_seconds and current_rss_bytes() < self.limits.rss_limit_bytes

    def reset(self) -> None:
        self.started = time.monotonic()
        self.used_seconds = 0.0


def current_rss_bytes() -> int:
    """Current RSS without a third-party dependency."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        class PMC(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
        counters = PMC(); counters.cb = ctypes.sizeof(counters)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        handle = kernel32.GetCurrentProcess()
        if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return int(counters.WorkingSetSize)
        return 0
    try:
        import resource
        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return int(rss if sys_platform_is_macos() else rss * 1024)
    except (ImportError, OSError):
        return 0


def sys_platform_is_macos() -> bool:
    import sys
    return sys.platform == "darwin"


def solve_root_jobs(jobs: Sequence[tuple[Any, Callable[[], SearchResult]]], *,
                    limits: ResourceLimits = ResourceLimits()) -> SearchResult:
    """Run independent root children on <=2 workers and combine safe bounds."""
    if not jobs: raise ValueError("root has no actions")
    results: list[tuple[Any, SearchResult]] = []
    with ThreadPoolExecutor(max_workers=min(2, limits.workers)) as pool:
        futures = {pool.submit(job): action for action, job in jobs}
        for future in as_completed(futures):
            results.append((futures[future], future.result()))
    action, best = min(results, key=lambda item: (-item[1].lower, repr(item[0])))
    upper = max(result.upper for _, result in results)
    certified = all(result.certified for _, result in results) and best.lower == upper
    return SearchResult(best.lower, upper, action, certified)

