package test_utils

import "testing"

func TestFallShortConnectionTimeoutMs(t *testing.T) {
	if got := fallShortConnectionTimeoutMs(3000, 0); got != 3000 {
		t.Fatalf("got %d", got)
	}
	if got := fallShortConnectionTimeoutMs(3000, 100); got != 400 {
		t.Fatalf("got %d", got)
	}
	if got := fallShortConnectionTimeoutMs(200, 100); got != 200 {
		t.Fatalf("got %d", got)
	}
}

func TestFallShortDownloadTimeoutMs(t *testing.T) {
	if got := fallShortDownloadTimeoutMs(5000, 0, 0); got != 5000 {
		t.Fatalf("got %d", got)
	}
	if got := fallShortDownloadTimeoutMs(5000, 400, 0); got != 1200 {
		t.Fatalf("got %d", got)
	}
	if got := fallShortDownloadTimeoutMs(5000, 400, 100); got != 400 {
		t.Fatalf("got %d", got)
	}
	if got := fallShortDownloadTimeoutMs(250, 400, 100); got != 250 {
		t.Fatalf("got %d", got)
	}
}
