package main

// C entry points for the embedded Tailscale node (deps/rclone-mobile/tsnet).
// Functions returning char* return a malloc'd JSON string the caller frees
// with MagkTsFreeString: {"ok":true[,"result":...]} or {"ok":false,"error":"..."}.

/*
#include <stdlib.h>

typedef void (*magk_ts_log_fn)(const char *line);
static void magk_ts_call_log(magk_ts_log_fn fn, const char *line) { if (fn) fn(line); }
*/
import "C"

import (
	"encoding/json"
	"sync"
	"time"
	"unsafe"

	"github.com/Magk/magk-rclone/tsnet"
)

var (
	tsLogMu sync.Mutex
	tsLogFn C.magk_ts_log_fn
)

func tsResult(extra string, err error) *C.char {
	if err != nil {
		b, _ := json.Marshal(map[string]any{"ok": false, "error": err.Error()})
		return C.CString(string(b))
	}
	if extra != "" {
		return C.CString(`{"ok":true,"result":` + extra + `}`)
	}
	return C.CString(`{"ok":true}`)
}

// MagkTsSetLogger registers where the node's log lines go (the app forwards
// them to AppLogger). NULL turns logging off.
//
//export MagkTsSetLogger
func MagkTsSetLogger(fn C.magk_ts_log_fn) {
	tsLogMu.Lock()
	tsLogFn = fn
	tsLogMu.Unlock()
	if fn == nil {
		tsnet.SetLogger(nil)
		return
	}
	tsnet.SetLogger(func(line string) {
		tsLogMu.Lock()
		f := tsLogFn
		tsLogMu.Unlock()
		cs := C.CString(line)
		C.magk_ts_call_log(f, cs)
		C.free(unsafe.Pointer(cs))
	})
}

// MagkTsSetDefaultRouteInterface passes the OS default interface name (from
// NWPathMonitor, e.g. "en0" / "pdp_ip0") to the node's network monitor.
//
//export MagkTsSetDefaultRouteInterface
func MagkTsSetDefaultRouteInterface(ifName *C.char) {
	tsnet.SetDefaultRouteInterface(C.GoString(ifName))
}

// MagkTsStart starts the node. authKey may be empty (interactive login; the
// AuthURL then shows up in MagkTsStatus).
//
//export MagkTsStart
func MagkTsStart(authKey, hostname, stateDir *C.char) *C.char {
	return tsResult("", tsnet.Start(C.GoString(authKey), C.GoString(hostname), C.GoString(stateDir)))
}

// MagkTsSetAcceptRoutes accepts (1) or ignores (0) subnet routes that
// peers advertise. Tailscale defaults to accepting; the app sets its own
// setting (default off) after MagkTsStart.
//
//export MagkTsSetAcceptRoutes
func MagkTsSetAcceptRoutes(on C.int) *C.char {
	return tsResult("", tsnet.SetAcceptRoutes(on != 0))
}

// MagkTsStop stops the SOCKS listener and the node (the login is kept).
//
//export MagkTsStop
func MagkTsStop() *C.char {
	return tsResult("", tsnet.Stop())
}

// MagkTsLogout signs the node out and stops it.
//
//export MagkTsLogout
func MagkTsLogout() *C.char {
	return tsResult("", tsnet.Logout())
}

// MagkTsStatus returns the node status as JSON.
//
//export MagkTsStatus
func MagkTsStatus() *C.char {
	return tsResult(tsnet.StatusJSON(), nil)
}

// MagkTsRoutes returns the prefixes currently reachable through the tailnet
// and the MagicDNS suffix.
//
//export MagkTsRoutes
func MagkTsRoutes() *C.char {
	return tsResult(tsnet.RoutesJSON(), nil)
}

// MagkTsDialTest dials addr ("host:port") through the tailnet.
//
//export MagkTsDialTest
func MagkTsDialTest(addr *C.char, timeoutMs C.int) *C.char {
	d, err := tsnet.DialTest(C.GoString(addr), time.Duration(timeoutMs)*time.Millisecond)
	if err != nil {
		return tsResult("", err)
	}
	b, _ := json.Marshal(map[string]any{"ms": d.Milliseconds()})
	return tsResult(string(b), nil)
}

// MagkTsListenSocks starts a local SOCKS5 proxy on addr ("127.0.0.1:0" picks
// a port). The result carries the bound address and generated credentials.
//
//export MagkTsListenSocks
func MagkTsListenSocks(addr *C.char) *C.char {
	return tsResult(tsnet.ListenSocks(C.GoString(addr)))
}

// MagkTsFreeString frees a string returned by the functions above.
//
//export MagkTsFreeString
func MagkTsFreeString(s *C.char) {
	C.free(unsafe.Pointer(s))
}
