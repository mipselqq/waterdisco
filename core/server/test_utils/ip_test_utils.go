package test_utils

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"strings"
	"time"

	"ThroneCore/internal/boxbox"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing/common/metadata"
)

type IPInfo struct {
	IP          string `json:"ip"`
	CountryCode string `json:"country_code"`
}

var IPReporter resultBuffer[IPTestResult]

const IPTestTimeout = 3 * time.Second
const liveIPTestTimeout = 5 * time.Second
const liveIPRetryDelay = 200 * time.Millisecond

// 1.1.1.1 is an IP literal so the probe needs no DNS. Cloudflare trace is the
// same family as ranked TTFB. ipify is JSON fallback. ip2location/ip.sb are
// omitted: they produced live-probe EOF while the tunnel still carried traffic.
var ipInfoAPIs = []string{
	"https://1.1.1.1/cdn-cgi/trace",
	"https://speed.cloudflare.com/cdn-cgi/trace",
	"https://api.ipify.org?format=json",
}

type IPTestResult struct {
	Result IPInfo
	Tag    string
	Error  error
}

func BatchIPTest(ctx context.Context, i *boxbox.Box, outboundTags []string, maxConcurrency int, timeout time.Duration, live bool) []*IPTestResult {
	if timeout <= 0 {
		timeout = IPTestTimeout
	}
	if live && timeout < liveIPTestTimeout {
		timeout = liveIPTestTimeout
	}
	// Drop a previous poller's leftovers; tags repeat on the live probe.
	_ = IPReporter.Results()

	results := runBatch(ctx, i, outboundTags, maxConcurrency, batchProbe[IPTestResult]{
		run: func(ctx context.Context, tag string, outbound adapter.Outbound) *IPTestResult {
			client := outboundHTTPClient(ctx, outbound, timeout)
			if live {
				client = liveOutboundHTTPClient(outbound, timeout)
			}
			info, err := ipTest(ctx, client, live)
			return &IPTestResult{Result: info, Tag: tag, Error: err}
		},
		fail: func(tag string, err error) *IPTestResult {
			return &IPTestResult{Tag: tag, Error: err}
		},
		publish: IPReporter.AddResult,
	})
	IPReporter.Reclaim(results)
	return results
}

// Opens a new TCP stream on the already-running Hysteria/sing-box client.
// Hop/EOF is retried on that same client instead of building a second tunnel.
func liveOutboundHTTPClient(outbound adapter.Outbound, timeout time.Duration) *http.Client {
	return dialerHTTPClient(func(reqCtx context.Context, network, addr string) (net.Conn, error) {
		var last error
		for attempt := 0; attempt < 3; attempt++ {
			if err := reqCtx.Err(); err != nil {
				return nil, err
			}
			conn, err := outbound.DialContext(reqCtx, "tcp", metadata.ParseSocksaddr(addr))
			if err == nil {
				return conn, nil
			}
			last = err
			if !isTransientHTTPErr(err) {
				return nil, err
			}
			timer := time.NewTimer(liveIPRetryDelay)
			select {
			case <-reqCtx.Done():
				timer.Stop()
				return nil, reqCtx.Err()
			case <-timer.C:
			}
		}
		return nil, last
	}, timeout)
}

func ipTest(ctx context.Context, client *http.Client, live bool) (IPInfo, error) {
	attempts := 1
	if live {
		attempts = 3
	}
	var lastErr error
	for attempt := 0; attempt < attempts; attempt++ {
		if attempt > 0 {
			timer := time.NewTimer(liveIPRetryDelay)
			select {
			case <-ctx.Done():
				timer.Stop()
				if lastErr == nil {
					lastErr = ctx.Err()
				}
				return IPInfo{}, lastErr
			case <-timer.C:
			}
		}
		for _, endpoint := range ipInfoAPIs {
			info, err := fetchIPInfo(ctx, client, endpoint)
			if err == nil && info.IP != "" {
				return info, nil
			}
			if err == nil {
				err = errors.New("empty IP in response")
			}
			lastErr = err
			if ctx.Err() != nil {
				return IPInfo{}, lastErr
			}
		}
	}
	if lastErr == nil {
		lastErr = errors.New("all IP endpoints failed")
	}
	return IPInfo{}, lastErr
}

func isTransientHTTPErr(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
		return true
	}
	msg := err.Error()
	return strings.Contains(msg, "EOF") || strings.Contains(msg, "connection reset")
}

func fetchIPInfo(ctx context.Context, client *http.Client, endpoint string) (IPInfo, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", endpoint, nil)
	if err != nil {
		return IPInfo{}, err
	}
	req.Header.Set("User-Agent", "Mozilla/5.0")
	req.Header.Set("Accept", "*/*")
	resp, err := client.Do(req)
	if err != nil {
		return IPInfo{}, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	if err != nil {
		return IPInfo{}, err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return IPInfo{}, errors.New(resp.Status)
	}
	if info, ok := parseCloudflareTrace(data); ok {
		return info, nil
	}
	info, err := parseIPJSON(data)
	if err != nil {
		return IPInfo{}, err
	}
	if info.IP == "" {
		return IPInfo{}, errors.New("empty IP in response")
	}
	return info, nil
}

func parseCloudflareTrace(data []byte) (IPInfo, bool) {
	if !bytes.Contains(data, []byte("ip=")) {
		return IPInfo{}, false
	}
	var info IPInfo
	scanner := bufio.NewScanner(bytes.NewReader(data))
	for scanner.Scan() {
		key, val, ok := strings.Cut(strings.TrimSpace(scanner.Text()), "=")
		if !ok {
			continue
		}
		switch key {
		case "ip":
			info.IP = val
		case "loc":
			info.CountryCode = val
		}
	}
	return info, info.IP != ""
}

func parseIPJSON(data []byte) (IPInfo, error) {
	var raw map[string]any
	if err := json.Unmarshal(data, &raw); err != nil {
		return IPInfo{}, err
	}
	var info IPInfo
	info.IP = jsonString(raw, "ip", "query", "origin")
	info.CountryCode = jsonString(raw, "country_code", "countryCode", "country_iso", "country")
	if len(info.CountryCode) > 2 {
		info.CountryCode = ""
	}
	return info, nil
}

func jsonString(raw map[string]any, keys ...string) string {
	for _, key := range keys {
		if v, ok := raw[key]; ok {
			if s, ok := v.(string); ok {
				return strings.TrimSpace(s)
			}
		}
	}
	return ""
}
