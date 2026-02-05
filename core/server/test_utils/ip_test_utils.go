package test_utils

import (
	"Core/internal/boxbox"
	"context"
	"encoding/json"
	"errors"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing/common/metadata"
	"github.com/sagernet/sing/service"
	"io"
	"net"
	"net/http"
	"sync"
	"time"
)

type IPInfo struct {
	IP          string `json:"ip"`
	CountryCode string `json:"country_code"`
}

var IPReporter IPTestReporter

const IPTestTimeout = 3 * time.Second
const ipInfoAPI = "https://api.ip2location.io/"

type IPTestResult struct {
	Result IPInfo
	Tag    string
	Error  error
}

type IPTestReporter struct {
	results []*IPTestResult
	mu      sync.Mutex
}

func (u *IPTestReporter) AddResult(result *IPTestResult) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.results = append(u.results, result)
}

func (u *IPTestReporter) Results() []*IPTestResult {
	u.mu.Lock()
	defer u.mu.Unlock()
	res := u.results
	u.results = nil
	return res
}

func BatchIPTest(ctx context.Context, i *boxbox.Box, outboundTags []string, maxConcurrency int, timeout time.Duration) []*IPTestResult {
	if timeout <= 0 {
		timeout = IPTestTimeout
	}
	outbounds := service.FromContext[adapter.OutboundManager](i.Context())
	resMap := make(map[string]*IPTestResult)
	resAccess := sync.Mutex{}
	limiter := make(chan struct{}, maxConcurrency)

	wg := &sync.WaitGroup{}
	wg.Add(len(outboundTags))
	for _, tag := range outboundTags {
		select {
		case <-ctx.Done():
			wg.Done()
			resAccess.Lock()
			resMap[tag] = &IPTestResult{
				Error: errors.New("test aborted"),
			}
			resAccess.Unlock()
		default:
			time.Sleep(2 * time.Millisecond) // don't spawn goroutines too quickly
			limiter <- struct{}{}
			go func(t string) {
				defer wg.Done()
				outbound, found := outbounds.Outbound(t)
				if !found {
					panic("no outbound with tag " + t + " found")
				}
				client := &http.Client{
					Transport: &http.Transport{
						DialContext: func(_ context.Context, network string, addr string) (net.Conn, error) {
							return outbound.DialContext(ctx, "tcp", metadata.ParseSocksaddr(addr))
						},
					},
					Timeout: timeout,
				}
				resp, err := ipTest(ctx, client)
				resAccess.Lock()
				u := &IPTestResult{
					Result: resp,
					Tag:    t,
					Error:  err,
				}
				resMap[t] = u
				IPReporter.AddResult(u)
				resAccess.Unlock()
				<-limiter
			}(tag)
		}
	}

	wg.Wait()
	res := make([]*IPTestResult, 0, len(outboundTags))
	for _, tag := range outboundTags {
		res = append(res, resMap[tag])
	}

	return res
}

func ipTest(ctx context.Context, client *http.Client) (IPInfo, error) {
	var res IPInfo
	req, err := http.NewRequestWithContext(ctx, "GET", ipInfoAPI, nil)
	if err != nil {
		return res, err
	}
	resp, err := client.Do(req)
	if err != nil {
		return res, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return res, err
	}
	err = json.Unmarshal(data, &res)
	if err != nil {
		return res, err
	}
	return res, nil
}
