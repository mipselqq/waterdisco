package rpc

import (
	"context"
	"errors"
	"time"

	"ThroneCore/gen"
	"ThroneCore/internal/probe"
)

func (s *server) Test(ctx context.Context, in *gen.TestReq) (*gen.TestResp, error) {
	env, err := prepareTestEnv(in.GetTestCurrent(), in.GetNeedXray(), in.GetXrayConfig(),
		in.XrayFullConfigs, in.GetConfig(), in.OutboundTags, in.GetUseDefaultOutbound(),
		in.GetXrayOutboundDnsStrategy())
	if err != nil {
		if errors.Is(err, errInstanceNotRunning) {
			return &gen.TestResp{Results: []*gen.URLTestResp{{
				OutboundTag: To("proxy"),
				LatencyMs:   To(int32(0)),
				Error:       To(err.Error()),
			}}}, nil
		}
		return nil, err
	}
	defer env.close()

	// Held, not re-read: StopTest rearms a fresh context, uncancelled.
	testCtx := probe.TestContext()
	if in.GetTestCurrent() {
		testCtx = probe.LiveInstance(testCtx)
	}

	// A muxed config needs a warm connection; the live instance already is one.
	twice := !in.GetTestCurrent()
	results := probe.BatchURLTest(testCtx, env.box, env.tags, in.GetUrl(),
		int(in.GetMaxConcurrency()), twice, time.Duration(in.GetTestTimeoutMs())*time.Millisecond,
		in.GetDynamicFallShort(), int64(in.GetFallShortBestMs()))

	res := make([]*gen.URLTestResp, 0, len(results))
	failed := make(map[string]bool, len(results))
	for idx, data := range results {
		errStr := ""
		if data.Error != nil {
			errStr = data.Error.Error()
		}
		failed[env.tags[idx]] = errStr != ""
		res = append(res, &gen.URLTestResp{
			OutboundTag: To(env.tags[idx]),
			LatencyMs:   To(int32(data.Duration.Milliseconds())),
			Error:       To(errStr),
		})
	}

	out := &gen.TestResp{Results: res}
	// `defer env.close()` tears the box down on return, so this cannot wait.
	var pending []string
	for _, tag := range in.VpnEndpointTags {
		if failed[tag] {
			pending = append(pending, tag)
		}
	}
	if len(pending) > 0 {
		timeout := time.Duration(in.GetVpnStatusTimeoutMs()) * time.Millisecond
		out.VpnStatus = collectVPNStatus(testCtx, env.box, pending, timeout)
	}
	return out, nil
}

func (s *server) StopTest(ctx context.Context, in *gen.EmptyReq) (*gen.EmptyResp, error) {
	probe.CancelTests()

	return &gen.EmptyResp{}, nil
}

func (s *server) QueryURLTest(ctx context.Context, in *gen.EmptyReq) (out *gen.QueryURLTestResponse, _ error) {
	results := probe.URLReporter.Results()
	out = &gen.QueryURLTestResponse{}
	for _, r := range results {
		errStr := ""
		if r.Error != nil {
			errStr = r.Error.Error()
		}
		out.Results = append(out.Results, &gen.URLTestResp{
			OutboundTag: To(r.Tag),
			LatencyMs:   To(int32(r.Duration.Milliseconds())),
			Error:       To(errStr),
		})
	}
	return
}

func (s *server) IPTest(ctx context.Context, in *gen.IPTestRequest) (*gen.IPTestResp, error) {
	// The Info panel probes the already-running proxy. A second box would not be
	// the tunnel that is carrying traffic, and split routing could leak the host IP.
	live := in.GetTestCurrent()
	env, err := prepareTestEnv(live, in.GetNeedXray(), in.GetXrayConfig(),
		in.XrayFullConfigs, in.GetConfig(), in.OutboundTags, in.GetUseDefaultOutbound(),
		in.GetXrayOutboundDnsStrategy())
	if err != nil {
		return nil, err
	}
	defer env.close()

	timeout := time.Duration(in.GetTestTimeoutMs()) * time.Millisecond
	probeCtx := probe.TestContext()
	if live {
		if timeout < 5*time.Second {
			timeout = 5 * time.Second
		}
		var cancel context.CancelFunc
		probeCtx, cancel = context.WithTimeout(context.Background(), timeout)
		defer cancel()
	}
	results := probe.BatchIPTest(probeCtx, env.box, env.tags,
		int(in.GetMaxConcurrency()), !live, timeout, live)

	res := make([]*gen.IPTestRes, 0, len(results))
	for idx, data := range results {
		errStr := ""
		if data.Error != nil {
			errStr = data.Error.Error()
		}
		res = append(res, &gen.IPTestRes{
			OutboundTag: To(env.tags[idx]),
			Ip:          To(data.Result.IP),
			CountryCode: To(data.Result.CountryCode),
			Error:       To(errStr),
		})
	}
	return &gen.IPTestResp{Results: res}, nil
}

func (s *server) QueryIPTest(ctx context.Context, in *gen.EmptyReq) (out *gen.QueryIPTestResponse, _ error) {
	results := probe.IPReporter.Results()
	out = &gen.QueryIPTestResponse{}
	for _, r := range results {
		errStr := ""
		if r.Error != nil {
			errStr = r.Error.Error()
		}
		out.Results = append(out.Results, &gen.IPTestRes{
			OutboundTag: To(r.Tag),
			Ip:          To(r.Result.IP),
			CountryCode: To(r.Result.CountryCode),
			Error:       To(errStr),
		})
	}
	return
}
