package main

import (
	"encoding/json"
	"runtime"
	"testing"

	tun "github.com/sagernet/sing-tun"
)

func TestConfigAutoRedirectMark(t *testing.T) {
	for _, tc := range []struct {
		name   string
		config string
		want   uint32
	}{
		{
			name:   "no inbounds at all",
			config: `{"outbounds":[{"type":"direct","tag":"direct"}]}`,
			want:   0,
		},
		{
			name:   "tun without auto_redirect",
			config: `{"inbounds":[{"type":"tun","tag":"tun-in","auto_route":true}]}`,
			want:   0,
		},
		{
			name:   "system proxy only",
			config: `{"inbounds":[{"type":"mixed","tag":"mixed-in","listen_port":2080}]}`,
			want:   0,
		},
		{
			name:   "tun with auto_redirect takes sing-tun's default",
			config: `{"inbounds":[{"type":"tun","tag":"tun-in","auto_route":true,"auto_redirect":true}]}`,
			want:   tun.DefaultAutoRedirectOutputMark,
		},
		{
			name:   "explicit output mark wins over the default",
			config: `{"inbounds":[{"type":"tun","auto_redirect":true,"auto_redirect_output_mark":4660}]}`,
			want:   4660,
		},
		{
			// option.FwMark marshals as a hex string, so a round-tripped config comes back in that form.
			name:   "hex string output mark",
			config: `{"inbounds":[{"type":"tun","auto_redirect":true,"auto_redirect_output_mark":"0x1234"}]}`,
			want:   4660,
		},
		{
			name:   "tun found behind other inbounds",
			config: `{"inbounds":[{"type":"direct","tag":"dns-in"},{"type":"mixed"},{"type":"tun","auto_redirect":true}]}`,
			want:   tun.DefaultAutoRedirectOutputMark,
		},
		{
			name:   "unexpected inbound shape is skipped, not fatal",
			config: `{"inbounds":[{"type":"tun","auto_redirect":["nonsense"]},{"type":"tun","auto_redirect":true}]}`,
			want:   tun.DefaultAutoRedirectOutputMark,
		},
		{
			name:   "malformed json",
			config: `{"inbounds":[`,
			want:   0,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := configAutoRedirectMark([]byte(tc.config)); got != tc.want {
				t.Errorf("configAutoRedirectMark() = %d, want %d", got, tc.want)
			}
		})
	}
}

func TestAutoRedirectMarkForIsLinuxOnly(t *testing.T) {
	config := []byte(`{"inbounds":[{"type":"tun","auto_redirect":true}]}`)
	want := uint32(0)
	if runtime.GOOS == "linux" {
		want = tun.DefaultAutoRedirectOutputMark
	}
	if got := autoRedirectMarkFor(config); got != want {
		t.Errorf("autoRedirectMarkFor() on %s = %d, want %d", runtime.GOOS, got, want)
	}
}

func TestStampHostEgress(t *testing.T) {
	const untouched = `{"route":{"auto_detect_interface":true}}`
	if got := stampHostEgress(untouched, "", 0); got != untouched {
		t.Errorf("no iface/mark rewrote config: %s", got)
	}
	if got := stampHostEgress("", "eth0", 4660); got != "" {
		t.Errorf("empty config became %q", got)
	}
	if got := stampHostEgress("{", "eth0", 0); got != "{" {
		t.Errorf("malformed JSON was rewritten: %q", got)
	}

	got := stampHostEgress(`{"route":{"auto_detect_interface":true},"outbounds":[{"type":"direct"}]}`, "eth0", 4660)
	var obj map[string]any
	if err := json.Unmarshal([]byte(got), &obj); err != nil {
		t.Fatalf("rewritten config is not JSON: %v", err)
	}
	route, _ := obj["route"].(map[string]any)
	if route == nil {
		t.Fatal("route missing")
	}
	if route["auto_detect_interface"] != false {
		t.Errorf("auto_detect_interface = %v, want false", route["auto_detect_interface"])
	}
	if route["default_interface"] != "eth0" {
		t.Errorf("default_interface = %v, want eth0", route["default_interface"])
	}
	mark, _ := route["default_mark"].(float64)
	if mark != 4660 {
		t.Errorf("default_mark = %v, want 4660", route["default_mark"])
	}

	ifaceOnly := stampHostEgress(`{"outbounds":[{"type":"direct"}]}`, "wlan0", 0)
	if err := json.Unmarshal([]byte(ifaceOnly), &obj); err != nil {
		t.Fatalf("iface-only config is not JSON: %v", err)
	}
	route, _ = obj["route"].(map[string]any)
	if route["default_interface"] != "wlan0" {
		t.Errorf("iface-only default_interface = %v", route["default_interface"])
	}
	if _, hasMark := route["default_mark"]; hasMark {
		t.Errorf("iface-only unexpectedly set default_mark")
	}

	markOnly := stampHostEgress(`{"route":{"auto_detect_interface":true}}`, "", 99)
	if err := json.Unmarshal([]byte(markOnly), &obj); err != nil {
		t.Fatalf("mark-only config is not JSON: %v", err)
	}
	route, _ = obj["route"].(map[string]any)
	mark, _ = route["default_mark"].(float64)
	if mark != 99 {
		t.Errorf("mark-only default_mark = %v, want 99", route["default_mark"])
	}
	if route["auto_detect_interface"] != true {
		t.Errorf("mark-only should keep auto_detect_interface, got %v", route["auto_detect_interface"])
	}
}
