package test_utils

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const diagFileName = "logs.txt"
const diagEnv = "THRONE_DIAG_LOG"

var (
	diagMu   sync.Mutex
	diagFile *os.File
)

func diagPath() string {
	if p := os.Getenv(diagEnv); p != "" {
		return p
	}
	if home, err := os.UserHomeDir(); err == nil && home != "" {
		return filepath.Join(home, "Desktop", diagFileName)
	}
	return filepath.Join("/home/lord/Desktop", diagFileName)
}

func Diag(format string, args ...any) {
	diagMu.Lock()
	defer diagMu.Unlock()
	if diagFile == nil {
		f, err := os.OpenFile(diagPath(), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
		if err != nil {
			return
		}
		diagFile = f
	}
	msg := fmt.Sprintf(format, args...)
	_, _ = fmt.Fprintf(diagFile, "%s [GO] %s\n", time.Now().Format("2006-01-02 15:04:05.000"), msg)
}
