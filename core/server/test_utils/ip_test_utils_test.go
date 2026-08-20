package test_utils

import "testing"

func TestParseCloudflareTrace(t *testing.T) {
	body := []byte("fl=1\nh=speed.cloudflare.com\nip=203.0.113.9\nloc=RU\ntls=TLSv1.3\n")
	info, ok := parseCloudflareTrace(body)
	if !ok || info.IP != "203.0.113.9" || info.CountryCode != "RU" {
		t.Fatalf("got %+v ok=%v", info, ok)
	}
}

func TestParseIPJSON(t *testing.T) {
	info, err := parseIPJSON([]byte(`{"ip":"198.51.100.4","country_code":"PL"}`))
	if err != nil || info.IP != "198.51.100.4" || info.CountryCode != "PL" {
		t.Fatalf("got %+v err=%v", info, err)
	}
	info, err = parseIPJSON([]byte(`{"query":"198.51.100.5","country":"Germany"}`))
	if err != nil || info.IP != "198.51.100.5" || info.CountryCode != "" {
		t.Fatalf("country name must not be stored as a code: %+v err=%v", info, err)
	}
}
