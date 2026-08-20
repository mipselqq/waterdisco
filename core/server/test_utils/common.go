package test_utils

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"sync"
	"sync/atomic"
	"time"

	"ThroneCore/internal/boxbox"

	"github.com/Mahdi-zarei/speedtest-go/speedtest"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing/common/metadata"
	"github.com/sagernet/sing/service"
)

const FetchServersTimeout = 8 * time.Second
const MaxConcurrentTests = 100

// The GUI matches on this text, so the wording is part of the contract.
var ErrTestAborted = errors.New("test aborted")

// Scopes every probe in flight so StopTest can cancel them together.
type testSession struct {
	mu     sync.Mutex
	ctx    context.Context
	cancel context.CancelFunc
}

var session testSession

func (s *testSession) current() context.Context {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.ctx == nil {
		s.ctx, s.cancel = context.WithCancel(context.Background())
	}
	return s.ctx
}

func (s *testSession) cancelAndRearm() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.cancel != nil {
		s.cancel()
	}
	s.ctx, s.cancel = context.WithCancel(context.Background())
}

func TestContext() context.Context { return session.current() }

// Aborts everything in flight and arms a fresh context for the next run.
func CancelTests() { session.cancelAndRearm() }

// Drains on read: each result is handed to the GUI exactly once.
type resultBuffer[T any] struct {
	results []*T
	mu      sync.Mutex
}

func (b *resultBuffer[T]) AddResult(result *T) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.results = append(b.results, result)
}

func (b *resultBuffer[T]) Results() []*T {
	b.mu.Lock()
	defer b.mu.Unlock()
	res := b.results
	b.results = nil
	return res
}

// Anything left buffered is drained by the next test, whose tags repeat ours.
func (b *resultBuffer[T]) Reclaim(owned []*T) {
	if len(owned) == 0 {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if len(b.results) == 0 {
		return
	}
	drop := make(map[*T]struct{}, len(owned))
	for _, r := range owned {
		drop[r] = struct{}{}
	}
	kept := b.results[:0]
	for _, r := range b.results {
		if _, ours := drop[r]; !ours {
			kept = append(kept, r)
		}
	}
	b.results = kept
}

// The per-kind half of runBatch: measure, report an unmeasurable tag, publish.
type batchProbe[T any] struct {
	run     func(ctx context.Context, tag string, outbound adapter.Outbound) *T
	fail    func(tag string, err error) *T
	publish func(*T)
}

func normalizeConcurrency(maxConcurrency int) int {
	if maxConcurrency <= 0 {
		return MaxConcurrentTests
	}
	return maxConcurrency
}

// One result per tag, in the order given. Cancelling ctx aborts tags not yet started.
func runBatch[T any](ctx context.Context, i *boxbox.Box, outboundTags []string, maxConcurrency int, probe batchProbe[T]) []*T {
	outbounds := service.FromContext[adapter.OutboundManager](i.Context())
	resMap := make(map[string]*T, len(outboundTags))
	var resAccess sync.Mutex
	conc := normalizeConcurrency(maxConcurrency)
	limiter := make(chan struct{}, conc)
	Diag("RUN_BATCH tags=%d concurrency=%d ctxErr=%v", len(outboundTags), conc, ctx.Err())

	store := func(tag string, res *T) {
		resAccess.Lock()
		resMap[tag] = res
		resAccess.Unlock()
	}

	wg := &sync.WaitGroup{}
	wg.Add(len(outboundTags))
	startedAt := time.Now()
	var started atomic.Int32
	var abortedBeforeStart atomic.Int32
	for _, tag := range outboundTags {
		select {
		case <-ctx.Done():
			Diag("RUN_BATCH_ABORT_BEFORE_START tag=%s elapsedMs=%d", tag, time.Since(startedAt).Milliseconds())
			store(tag, probe.fail(tag, ErrTestAborted))
			wg.Done()
			abortedBeforeStart.Add(1)
			continue
		default:
		}

		waitLimiter := time.Now()
		limiter <- struct{}{}
		waitMs := time.Since(waitLimiter).Milliseconds()
		if waitMs > 5 {
			Diag("RUN_BATCH_LIMITER_WAIT tag=%s waitMs=%d", tag, waitMs)
		}
		started.Add(1)
		go func(t string) {
			defer wg.Done()
			defer func() { <-limiter }()

			outbound, found := outbounds.Outbound(t)
			if !found {
				Diag("RUN_BATCH_NO_OUTBOUND tag=%s", t)
				res := probe.fail(t, fmt.Errorf("no outbound with tag %s found", t))
				store(t, res)
				probe.publish(res)
				return
			}
			probeStart := time.Now()
			res := probe.run(ctx, t, outbound)
			if ctx.Err() != nil {
				Diag("RUN_BATCH_CTX_CANCEL_AFTER tag=%s probeMs=%d ctxErr=%v", t, time.Since(probeStart).Milliseconds(), ctx.Err())
				res = probe.fail(t, ErrTestAborted)
			}
			store(t, res)
			if ctx.Err() == nil {
				probe.publish(res)
			}
		}(tag)
	}

	wg.Wait()
	Diag("RUN_BATCH_DONE tags=%d started=%d abortedBeforeStart=%d elapsedMs=%d",
		len(outboundTags), started.Load(), abortedBeforeStart.Load(), time.Since(startedAt).Milliseconds())

	res := make([]*T, 0, len(outboundTags))
	for _, tag := range outboundTags {
		r, ok := resMap[tag]
		if !ok || r == nil {
			r = probe.fail(tag, errors.New("no result"))
		}
		res = append(res, r)
	}
	return res
}

// Routes every request through `dial`, passing the per-request context straight on.
func dialerHTTPClient(dial func(ctx context.Context, network, address string) (net.Conn, error), timeout time.Duration) *http.Client {
	return &http.Client{
		Transport: &http.Transport{
			// nil URL disables HTTP_PROXY; ProxyNone needs Go 1.23+.
			Proxy: func(*http.Request) (*url.URL, error) { return nil, nil },
			DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
				return dial(ctx, network, addr)
			},
			// Parallel URL tests must not leave idle tunnels around: 100 keep-alives
			// on a shared box inflate later TTFBs and trip fall-short.
			DisableKeepAlives: true,
			// HTTP/2 + DisableKeepAlives closes the connection under the first
			// response and surfaces as `Get "...": EOF` through Hysteria/QUIC even
			// when the tunnel is fine. Force HTTP/1.1 for every test probe.
			ForceAttemptHTTP2: false,
			TLSNextProto:      map[string]func(authority string, c *tls.Conn) http.RoundTripper{},
			TLSClientConfig:   &tls.Config{NextProtos: []string{"http/1.1"}},
		},
		Timeout: timeout,
	}
}

// Measures one outbound. Dial follows the HTTP request context so Client.Timeout
// and fall-short actually tear the proxy dial down; StopTest still wins because
// the request context is a child of the batch context.
func outboundHTTPClient(_ context.Context, outbound adapter.Outbound, timeout time.Duration) *http.Client {
	return dialerHTTPClient(func(reqCtx context.Context, network, addr string) (net.Conn, error) {
		dialStart := time.Now()
		conn, err := outbound.DialContext(reqCtx, "tcp", metadata.ParseSocksaddr(addr))
		Diag("DIAL tag=%s addr=%s durMs=%d reqCtxErr=%v err=%v",
			outbound.Tag(), addr, time.Since(dialStart).Milliseconds(), reqCtx.Err(), err)
		return conn, err
	}, timeout)
}

func getNetDialer(dialer func(ctx context.Context, network string, destination metadata.Socksaddr) (net.Conn, error)) func(ctx context.Context, network string, address string) (net.Conn, error) {
	return func(ctx context.Context, network string, address string) (net.Conn, error) {
		return dialer(ctx, network, metadata.ParseSocksaddr(address))
	}
}

func getSpeedtestServer(ctx context.Context, dialer func(ctx context.Context, network string, address string) (net.Conn, error)) (*speedtest.Server, error) {
	clt := speedtest.New(speedtest.WithUserConfig(&speedtest.UserConfig{
		DialContextFunc: dialer,
		PingMode:        speedtest.HTTP,
		MaxConnections:  8,
	}))
	fetchCtx, cancel := context.WithTimeout(ctx, FetchServersTimeout)
	defer cancel()
	srv, err := clt.FetchServerListContext(fetchCtx)
	if err != nil {
		return nil, err
	}
	srv, err = srv.FindServer(nil)
	if err != nil {
		return nil, err
	}

	if srv.Len() == 0 {
		return nil, errors.New("no server found for speedTest")
	}

	return srv[0], nil
}
