// +build ignore

package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"time"
)

func main() {
	// 1) many net.Interfaces() calls: sockets are created and closed over and over
	bad := 0
	for i := 0; i < 300; i++ {
		ifs, err := net.Interfaces()
		if err != nil || len(ifs) != 1 || ifs[0].Name != "lo" {
			bad++
		}
	}
	fmt.Println("300x net.Interfaces: bad =", bad)
	// 2) ordinary TCP + HTTP inside the sandbox must be untouched by the interception
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	go http.Serve(ln, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "hello-tcp") }))
	time.Sleep(100 * time.Millisecond)
	resp, err := http.Get("http://" + ln.Addr().String())
	if err != nil {
		fmt.Println("http error:", err)
		os.Exit(1)
	}
	b, _ := io.ReadAll(resp.Body)
	fmt.Println("http over loopback:", string(b))
	// 3) UDP echo
	pc, _ := net.ListenPacket("udp", "127.0.0.1:0")
	go func() { buf := make([]byte, 64); n, a, _ := pc.ReadFrom(buf); pc.WriteTo(buf[:n], a) }()
	c, _ := net.Dial("udp", pc.LocalAddr().String())
	c.Write([]byte("ping"))
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 16)
	n, err := c.Read(buf)
	fmt.Println("udp echo:", string(buf[:n]), err)
}
