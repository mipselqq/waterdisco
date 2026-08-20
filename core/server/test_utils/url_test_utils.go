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

func BatchURLTest(ctx context.Context, i *boxbox.Box, outboundTags []string, url string, maxConcurrency int, twice bool, timeout time.Duration, dynamicFallShort bool) []*URLTestResult {
	if timeout <= 0 {
		timeout = URLTestTimeout
	}

	var bestSuccessfulMs int64
	results := runBatch(ctx, i, outboundTags, maxConcurrency, batchProbe[URLTestResult]{
		run: func(ctx context.Context, tag string, outbound adapter.Outbound) *URLTestResult {
			probeCtx := ctx
			var cancel context.CancelFunc
			if dynamicFallShort {
				probeCtx, cancel = context.WithCancel(ctx)
				defer cancel()
				startAt := time.Now()
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
							threshold := 3 * bestMs
							if threshold < 1 {
								threshold = 1
							}
							if time.Since(startAt) > time.Duration(threshold)*time.Millisecond {
								cancel()
								return
							}
						}
					}
				}()
			}

			client := outboundHTTPClient(probeCtx, outbound, timeout)
			// to properly measure muxed configs, let's do the test twice
			duration, err := urlTest(probeCtx, client, url)
			if err == nil && twice {
				duration, err = urlTest(probeCtx, client, url)
			}
			if err == nil && dynamicFallShort {
				ms := duration.Milliseconds()
				if ms > 0 {
					for {
						current := atomic.LoadInt64(&bestSuccessfulMs)
						if current > 0 && current <= ms {
							break
						}
						if atomic.CompareAndSwapInt64(&bestSuccessfulMs, current, ms) {
							break
						}
					}
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
