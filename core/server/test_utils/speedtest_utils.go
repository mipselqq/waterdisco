package test_utils

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"ThroneCore/internal"
	"ThroneCore/internal/boxbox"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing/service"
)

var SpTQuerier SpeedTestResultQuerier
var CountryResults resultBuffer[SpeedTestResult]
var SpeedReporter resultBuffer[SpeedTestResult]

// How often a running speed test republishes its partial numbers for the GUI.
const speedtestSampleInterval = 50 * time.Millisecond

type SpeedTestResult struct {
	Tag           string
	DlSpeed       string
	UlSpeed       string
	Latency       int32
	ServerName    string
	ServerCountry string
	Error         error
	Cancelled     bool
	DlBytes       int64 // total bytes moved by the test, credited to per-config stats
	UlBytes       int64
	ElapsedMs     int64
}

type SpeedTestResultQuerier struct {
	isRunning bool
	current   SpeedTestResult
	mu        sync.RWMutex
}

func (s *SpeedTestResultQuerier) Result() (SpeedTestResult, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.current, s.isRunning
}

func (s *SpeedTestResultQuerier) storeResult(result *SpeedTestResult) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.current = *result
}

func (s *SpeedTestResultQuerier) setIsRunning(isRunning bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.isRunning = isRunning
}

// Tallies bytes without keeping them: the sampling loop reads the total mid-copy,
// which a bytes.Buffer cannot serve safely.
type countingWriter struct{ n atomic.Int64 }

func (w *countingWriter) Write(p []byte) (int, error) {
	w.n.Add(int64(len(p)))
	return len(p), nil
}

func countryTest(ctx context.Context, dialer func(ctx context.Context, network string, address string) (net.Conn, error), res *SpeedTestResult) error {
	srv, err := getSpeedtestServer(ctx, dialer)
	if err != nil {
		return err
	}
	res.ServerName = srv.Name
	res.ServerCountry = srv.Country
	res.Latency = int32(srv.Latency.Milliseconds())
	return nil
}

func BatchSpeedTest(ctx context.Context, i *boxbox.Box, outboundTags []string, testDl, testUl bool, simpleDL bool, simpleAddress string, timeout time.Duration, countryOnly bool, concurrency int32, dynamicFallShort bool, globalBestConnMs, globalBestDlMs int64) []*SpeedTestResult {
	outbounds := service.FromContext[adapter.OutboundManager](i.Context())
	results := make([]*SpeedTestResult, 0, len(outboundTags))
	if concurrency <= 0 {
		concurrency = 5
	}
	_ = SpeedReporter.Results()
	queuer := make(chan struct{}, concurrency)
	wg := &sync.WaitGroup{}
	bestConnMs := globalBestConnMs
	bestDlMs := globalBestDlMs

	for _, tag := range outboundTags {
		// A plain `break` here would leave the select, not the loop.
		if ctx.Err() != nil {
			break
		}

		res := new(SpeedTestResult)
		res.Tag = tag
		results = append(results, res)

		outbound, exists := outbounds.Outbound(tag)
		if !exists {
			// Report rather than panic: a tag can vanish between building the box and testing.
			res.Error = fmt.Errorf("no outbound with tag %s found", tag)
			if countryOnly {
				CountryResults.AddResult(res)
			} else {
				SpeedReporter.AddResult(res)
			}
			continue
		}

		queuer <- struct{}{}
		wg.Add(1)
		go func(res *SpeedTestResult, outbound adapter.Outbound) {
			defer func() { <-queuer }()
			defer wg.Done()
			if countryOnly {
				err := countryTest(ctx, getNetDialer(outbound.DialContext), res)
				if err != nil && !errors.Is(err, context.Canceled) {
					res.Error = err
					fmt.Println("Failed to countryTest with err:", err)
				}
				CountryResults.AddResult(res)
				return
			}
			var err error
			if simpleDL {
				probeTimeout := timeout
				if dynamicFallShort {
					probeTimeout = time.Duration(fallShortDownloadTimeoutMs(
						timeout.Milliseconds(), atomic.LoadInt64(&bestDlMs), atomic.LoadInt64(&bestConnMs))) * time.Millisecond
				}
				err = simpleDownloadTest(ctx, getNetDialer(outbound.DialContext), res, simpleAddress, probeTimeout,
					dynamicFallShort, timeout.Milliseconds(), &bestConnMs, &bestDlMs)
			} else {
				err = speedTestWithDialer(ctx, getNetDialer(outbound.DialContext), res, testDl, testUl, timeout)
			}
			if err == nil {
				noteFasterMs(&bestConnMs, int64(res.Latency))
				noteFasterMs(&bestDlMs, res.ElapsedMs)
			}
			if err != nil {
				if errors.Is(err, context.Canceled) && ctx.Err() != nil {
					res.Cancelled = true
				} else {
					res.Error = err
					if !errors.Is(err, context.Canceled) {
						fmt.Println("Failed to speedtest with err:", err)
					}
				}
			}
			if !testDl && !simpleDL {
				res.DlSpeed = ""
			}
			if !testUl {
				res.UlSpeed = ""
			}
			SpeedReporter.AddResult(res)
		}(res, outbound)
	}
	wg.Wait()
	CountryResults.Reclaim(results)
	SpeedReporter.Reclaim(results)

	return results
}

func simpleDownloadTest(ctx context.Context, dialer func(ctx context.Context, network string, address string) (net.Conn, error), res *SpeedTestResult, testURL string, timeout time.Duration, dynamicFallShort bool, configuredMs int64, bestConnMs, bestDlMs *int64) error {
	if timeout <= 0 {
		timeout = URLTestTimeout
	}
	client := dialerHTTPClient(dialer, timeout)

	res.ServerName = "N/A"
	res.ServerCountry = "N/A"

	probeCtx := ctx
	var cancel context.CancelFunc
	if dynamicFallShort {
		probeCtx, cancel = context.WithCancel(ctx)
		defer cancel()
	}

	req, err := http.NewRequestWithContext(probeCtx, "GET", testURL, nil)
	if err != nil {
		return err
	}

	done := make(chan struct{})
	// Written by the download goroutine while the sampling loop reads them.
	var counted countingWriter
	var startNs atomic.Int64
	var latencyMs atomic.Int32
	var elapsedMs atomic.Int64
	// Only read after <-done, which orders it against the goroutine's write.
	var downloadErr error
	reqStart := time.Now()

	if dynamicFallShort && cancel != nil {
		go func() {
			ticker := time.NewTicker(2 * time.Millisecond)
			defer ticker.Stop()
			for {
				select {
				case <-probeCtx.Done():
					return
				case <-ticker.C:
					limit := fallShortDownloadTimeoutMs(configuredMs, atomic.LoadInt64(bestDlMs), atomic.LoadInt64(bestConnMs))
					if time.Since(reqStart) > time.Duration(limit)*time.Millisecond {
						cancel()
						return
					}
				}
			}
		}()
	}

	go func() {
		defer close(done)
		resp, err := client.Do(req)
		if err != nil {
			elapsedMs.Store(time.Since(reqStart).Milliseconds())
			downloadErr = err
			return
		}
		defer resp.Body.Close()
		ttfb := time.Since(reqStart).Milliseconds()
		latencyMs.Store(int32(ttfb))
		noteFasterMs(bestConnMs, ttfb)
		startNs.Store(time.Now().UnixNano())
		_, copyErr := io.Copy(&counted, resp.Body)
		elapsedMs.Store(time.Since(reqStart).Milliseconds())
		if copyErr != nil {
			downloadErr = copyErr
		}
	}()

	ticker := time.NewTicker(speedtestSampleInterval)
	defer ticker.Stop()

	SpTQuerier.setIsRunning(true)
	defer SpTQuerier.setIsRunning(false)

	// Before the first byte there is no start time, so the rate stays untouched.
	sample := func() {
		n := counted.n.Load()
		if start := startNs.Load(); start != 0 {
			res.DlSpeed = internal.BrateToStr(internal.CalculateBRate(float64(n), time.Unix(0, start)))
		}
		res.DlBytes = n
		res.Latency = latencyMs.Load()
		res.ElapsedMs = elapsedMs.Load()
		SpTQuerier.storeResult(res)
	}

	for {
		select {
		case <-done:
			if downloadErr != nil {
				res.Error = downloadErr
			}
			sample()
			if res.ElapsedMs <= 0 {
				res.ElapsedMs = time.Since(reqStart).Milliseconds()
			}
			if downloadErr == nil {
				noteFasterMs(bestDlMs, res.ElapsedMs)
				noteFasterMs(bestConnMs, int64(res.Latency))
			}
			return downloadErr
		case <-probeCtx.Done():
			res.ElapsedMs = time.Since(reqStart).Milliseconds()
			if ctx.Err() != nil {
				res.Cancelled = true
				return ctx.Err()
			}
			return probeCtx.Err()
		case <-ticker.C:
			sample()
		}
	}
}

func speedTestWithDialer(ctx context.Context, dialer func(ctx context.Context, network string, address string) (net.Conn, error), res *SpeedTestResult, testDl, testUl bool, timeout time.Duration) error {
	srv, err := getSpeedtestServer(ctx, dialer)
	if err != nil {
		return err
	}
	res.ServerName = srv.Name
	res.ServerCountry = srv.Country

	done := make(chan struct{})
	// Only read after <-done; assigning the enclosing `err` in there raced.
	var testErr error

	SpTQuerier.setIsRunning(true)
	defer SpTQuerier.setIsRunning(false)

	go func() {
		defer func() { close(done) }()
		if testDl {
			timeoutCtx, cancel := context.WithTimeout(ctx, timeout)
			defer cancel()
			if e := srv.DownloadTestContext(timeoutCtx); e != nil && !errors.Is(e, context.Canceled) {
				testErr = e
				return
			}
		}
		if testUl {
			timeoutCtx, cancel := context.WithTimeout(ctx, timeout)
			defer cancel()
			if e := srv.UploadTestContext(timeoutCtx); e != nil && !errors.Is(e, context.Canceled) {
				testErr = e
				return
			}
		}
	}()

	ticker := time.NewTicker(speedtestSampleInterval)
	defer ticker.Stop()

	for {
		select {
		case <-done:
			if testErr != nil {
				res.Error = testErr
			}
			res.DlSpeed = internal.BrateToStr(float64(srv.DLSpeed))
			res.UlSpeed = internal.BrateToStr(float64(srv.ULSpeed))
			res.DlBytes = srv.Context.GetTotalDownload()
			res.UlBytes = srv.Context.GetTotalUpload()
			res.Latency = int32(srv.Latency.Milliseconds())
			SpTQuerier.storeResult(res)
			return nil
		case <-ctx.Done():
			res.Cancelled = true
			return ctx.Err()
		case <-ticker.C:
			res.DlSpeed = internal.BrateToStr(srv.Context.GetEWMADownloadRate())
			res.UlSpeed = internal.BrateToStr(srv.Context.GetEWMAUploadRate())
			res.DlBytes = srv.Context.GetTotalDownload()
			res.UlBytes = srv.Context.GetTotalUpload()
			res.Latency = int32(srv.Latency.Milliseconds())
			SpTQuerier.storeResult(res)
		}
	}
}
