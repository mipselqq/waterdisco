package test_utils

import (
	"context"
	"net/http"
	"sync/atomic"
	"time"

	"ThroneCore/internal/boxbox"

	"github.com/sagernet/sing-box/adapter"
)

var URLReporter resultBuffer[URLTestResult]

const URLTestTimeout = 3 * time.Second

type URLTestResult struct {
	Duration time.Duration
	Tag      string
	Error    error
}

func BatchURLTest(ctx context.Context, i *boxbox.Box, outboundTags []string, url string, maxConcurrency int, twice bool, timeout time.Duration, dynamicFallShort bool, globalBestMs int64) []*URLTestResult {
	if timeout <= 0 {
		timeout = URLTestTimeout
	}
	// Drop results a previous poller did not drain; tags like proxy-1-0 repeat
	// every ranked chunk and would otherwise be applied to the wrong profiles.
	_ = URLReporter.Results()

	bestSuccessfulMs := globalBestMs
	results := runBatch(ctx, i, outboundTags, maxConcurrency, batchProbe[URLTestResult]{
		run: func(ctx context.Context, tag string, outbound adapter.Outbound) *URLTestResult {
			client := outboundHTTPClient(ctx, outbound, timeout)
			runProbe := func() (time.Duration, error) {
				probeCtx := ctx
				var cancel context.CancelFunc
				startAt := time.Now()
				if dynamicFallShort {
					probeCtx, cancel = context.WithCancel(ctx)
					defer cancel()
					go func() {
						ticker := time.NewTicker(2 * time.Millisecond)
						defer ticker.Stop()
						for {
							select {
							case <-probeCtx.Done():
								return
							case <-ticker.C:
								bestMs := atomic.LoadInt64(&bestSuccessfulMs)
								if bestMs <= 0 {
									continue
								}
								threshold := fallShortConnectionTimeoutMs(timeout.Milliseconds(), bestMs)
								if time.Since(startAt) > time.Duration(threshold)*time.Millisecond {
									cancel()
									return
								}
							}
						}
					}()
				}
				return urlTest(probeCtx, client, url)
			}

			duration, err := runProbe()
			if err == nil {
				noteFasterMs(&bestSuccessfulMs, duration.Milliseconds())
			}
			if err == nil && twice {
				duration, err = runProbe()
				if err == nil {
					noteFasterMs(&bestSuccessfulMs, duration.Milliseconds())
				}
			}
			return &URLTestResult{Duration: duration, Tag: tag, Error: err}
		},
		fail: func(tag string, err error) *URLTestResult {
			return &URLTestResult{Tag: tag, Error: err}
		},
		publish: URLReporter.AddResult,
	})
	URLReporter.Reclaim(results)
	return results
}

func urlTest(ctx context.Context, client *http.Client, url string) (time.Duration, error) {
	begin := time.Now()
	req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
	if err != nil {
		return 0, err
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	_ = resp.Body.Close()
	return time.Since(begin), nil
}
