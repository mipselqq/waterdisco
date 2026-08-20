package test_utils

import "sync/atomic"

// Keep in lockstep with Configs::kRankedFallShort* in ProfileMetrics.h.
const (
	FallShortConnectionMultiplier = 4
	FallShortDownloadMultiplier   = 3
)

func noteFasterMs(best *int64, ms int64) {
	if best == nil || ms <= 0 {
		return
	}
	for {
		current := atomic.LoadInt64(best)
		if current > 0 && current <= ms {
			return
		}
		if atomic.CompareAndSwapInt64(best, current, ms) {
			return
		}
	}
}

func fallShortConnectionTimeoutMs(configured, bestConnectionMs int64) int64 {
	if configured < 1 {
		configured = 1
	}
	if bestConnectionMs <= 0 {
		return configured
	}
	capMs := bestConnectionMs * FallShortConnectionMultiplier
	if capMs < 1 {
		capMs = 1
	}
	if capMs < configured {
		return capMs
	}
	return configured
}

func fallShortDownloadTimeoutMs(configured, bestDownloadMs, bestConnectionMs int64) int64 {
	timeout := configured
	if timeout < 1 {
		timeout = 1
	}
	if bestDownloadMs > 0 {
		capMs := bestDownloadMs * FallShortDownloadMultiplier
		if capMs < 1 {
			capMs = 1
		}
		if capMs < timeout {
			timeout = capMs
		}
	}
	if bestConnectionMs > 0 {
		capMs := bestConnectionMs * FallShortConnectionMultiplier
		if capMs < 1 {
			capMs = 1
		}
		if capMs < timeout {
			timeout = capMs
		}
	}
	return timeout
}
