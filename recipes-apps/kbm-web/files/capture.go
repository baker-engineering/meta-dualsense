// Capture: SSE relay of raw EV_KEY events from dualsense-ffsd's
// /run/kbm-mapper/capture.sock to the web UI. Used by the "Press a key"
// binding helper so users don't have to memorize KEY_*/BTN_* names.

package main

import (
	"bufio"
	"fmt"
	"net"
	"net/http"
	"time"
)

const captureSockPath = "/run/kbm-mapper/capture.sock"

func handleCaptureSSE(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", 500)
		return
	}

	conn, err := net.DialTimeout("unix", captureSockPath, 2*time.Second)
	if err != nil {
		// Daemon may be in passthrough mode (different binary, no socket) or
		// briefly between restarts. Tell the client; they can retry.
		fmt.Fprintf(w, "event: unavailable\ndata: %s\n\n", err.Error())
		flusher.Flush()
		return
	}
	defer conn.Close()

	// If the client disconnects, the conn write below will EPIPE and we
	// return. Also tie the conn lifetime to the request context so we don't
	// leak a goroutine if the client drops.
	go func() {
		<-r.Context().Done()
		conn.Close()
	}()

	rdr := bufio.NewReader(conn)
	for {
		line, err := rdr.ReadString('\n')
		if err != nil {
			return
		}
		// Daemon emits one JSON object per line; relay as a single SSE
		// data: frame. The trailing newline is already in `line`.
		if _, err := fmt.Fprintf(w, "data: %s\n", line); err != nil {
			return
		}
		flusher.Flush()
	}
}
