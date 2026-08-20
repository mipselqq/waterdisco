package test_utils

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"
	"time"

	"ThroneCore/internal/boxbox"

	"github.com/sagernet/sing-box/adapter"
)

type IPInfo struct {
	IP          string `json:"ip"`
	CountryCode string `json:"country_code"`
}

var IPReporter resultBuffer[IPTestResult]

const IPTestTimeout = 3 * time.Second

// speed.cloudflare.com already answers the ranked TTFB probe on this VPN.
// 1.1.1.1 skips DNS. ipify is JSON fallback. ip2location/ip.sb are omitted:
// they are what produced the live-probe EOF while the tunnel still carried traffic.
var ipInfoAPIs = []string{
	"https://speed.cloudflare.com/cdn-cgi/trace",
	"https://1.1.1.1/cdn-cgi/trace",
	"https://api.ipify.org?format=json",
}

type IPTestResult struct {
	Result IPInfo
	Tag    string
	Error  error
}

func BatchIPTest(ctx context.Context, i *boxbox.Box, outboundTags []string, maxConcurrency int, timeout time.Duration) []*IPTestResult {
	if timeout <= 0 {
		timeout = IPTestTimeout
	}

	results := runBatch(ctx, i, outboundTags, maxConcurrency, batchProbe[IPTestResult]{
		run: func(ctx context.Context, tag string, outbound adapter.Outbound) *IPTestResult {
			client := outboundHTTPClient(ctx, outbound, timeout)
			info, err := ipTest(ctx, client)
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

func ipTest(ctx context.Context, client *http.Client) (IPInfo, error) {
	var lastErr error
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
			break
		}
		if isTransientHTTPErr(err) {
			info, err = fetchIPInfo(ctx, client, endpoint)
			if err == nil && info.IP != "" {
				return info, nil
			}
			if err != nil {
				lastErr = err
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
