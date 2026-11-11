package main

// C entry point for rcloneroute: SFTP and FTP remotes follow the backup
// scope of Settings > Network through the local SOCKS5 entry. Returns an
// envelope freed with MagkTsFreeString: {"ok":true,"result":{"changed":b}}.

/*
#include <stdlib.h>
*/
import "C"

import (
	"strconv"

	"github.com/Magk/magk-rclone/rcloneroute"
	"github.com/Magk/magk-rclone/tsnet"
)

// MagkRcloneSetSocks sets socks_proxy ("user:pass@host:port") for every
// SFTP and FTP remote created from now on, or unsets it with "". A change
// clears rclone's fs cache.
//
//export MagkRcloneSetSocks
func MagkRcloneSetSocks(socks *C.char) *C.char {
	changed, err := rcloneroute.SetSocks(C.GoString(socks), tsnet.Logf)
	return tsResult(`{"changed":`+strconv.FormatBool(changed)+`}`, err)
}
