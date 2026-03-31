package test_utils

import (
	"ThroneCore/internal/boxbox"
	"context"
	"errors"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing/common/metadata"
	"github.com/sagernet/sing/service"
)

var URLReporter URLTestReporter

const URLTestTimeout = 3 * time.Second

type URLTestResult struct {
	Duration time.Duration
	Tag      string
	Error    error
}

type URLTestReporter struct {
	results []*URLTestResult
	mu      sync.Mutex
}

func (u *URLTestReporter) AddResult(result *URLTestResult) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.results = append(u.results, result)
}

func (u *URLTestReporter) Results() []*URLTestResult {
	u.mu.Lock()
	defer u.mu.Unlock()
	res := u.results
	u.results = nil
	return res
}

func BatchURLTest(ctx context.Context, i *boxbox.Box, outboundTags []string, url string, maxConcurrency int, twice bool, timeout time.Duration, dynamicFallShort bool) []*URLTestResult {
	if timeout <= 0 {
		timeout = URLTestTimeout
	}
	outbounds := service.FromContext[adapter.OutboundManager](i.Context())
	resMap := make(map[string]*URLTestResult)
	resAccess := sync.Mutex{}
	limiter := make(chan struct{}, maxConcurrency)
	var bestSuccessfulMs int64

	wg := &sync.WaitGroup{}
	wg.Add(len(outboundTags))
	for _, tag := range outboundTags {
		select {
		case <-ctx.Done():
			wg.Done()
			resAccess.Lock()
			resMap[tag] = &URLTestResult{
				Duration: 0,
				Error:    errors.New("test aborted"),
			}
			resAccess.Unlock()
		default:
			limiter <- struct{}{}
			go func(t string) {
				defer wg.Done()
				outbound, found := outbounds.Outbound(t)
				if !found {
					panic("no outbound with tag " + t + " found")
				}
				client := &http.Client{
					Transport: &http.Transport{
						DialContext: func(innerCtx context.Context, network string, addr string) (net.Conn, error) {
							return outbound.DialContext(innerCtx, "tcp", metadata.ParseSocksaddr(addr))
						},
					},
					Timeout: timeout,
				}

				testCtx, testCancel := context.WithCancel(ctx)
				defer testCancel()
				if dynamicFallShort {
					startAt := time.Now()
					go func() {
						ticker := time.NewTicker(2 * time.Millisecond)
						defer ticker.Stop()
						for {
							select {
							case <-testCtx.Done():
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
									testCancel()
									return
								}
							}
						}
					}()
				}
				// to properly measure muxed configs, let's do the test twice
				duration, err := urlTest(testCtx, client, url)
				if err == nil && twice {
					duration, err = urlTest(testCtx, client, url)
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
				resAccess.Lock()
				u := &URLTestResult{
					Duration: duration,
					Tag:      t,
					Error:    err,
				}
				resMap[t] = u
				URLReporter.AddResult(u)
				resAccess.Unlock()
				<-limiter
			}(tag)
		}
	}

	wg.Wait()
	res := make([]*URLTestResult, 0, len(outboundTags))
	for _, tag := range outboundTags {
		res = append(res, resMap[tag])
	}

	return res
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
