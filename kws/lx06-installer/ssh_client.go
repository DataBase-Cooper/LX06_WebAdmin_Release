package main

// Minimal x/crypto/ssh client for LX06 dropbear (2017.75) provisioning.
// The device negotiates with the x/crypto v0.35 defaults proven by the
// sshtest harness (insecure host key, password auth, 8s timeout).

import (
	"bytes"
	"fmt"
	"net"
	"time"

	"golang.org/x/crypto/ssh"
)

type sshClient struct {
	addr   string
	client *ssh.Client
}

func sshDial(host string, port int, user, password string, timeout time.Duration) (*sshClient, error) {
	if net.ParseIP(host) == nil {
		return nil, fmt.Errorf("设备 IP 地址无效: %q", host)
	}
	if user == "" {
		user = "root"
	}
	cfg := &ssh.ClientConfig{
		User:            user,
		Auth:            []ssh.AuthMethod{ssh.Password(password)},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
		Timeout:         timeout,
	}
	addr := net.JoinHostPort(host, fmt.Sprintf("%d", port))
	d := &net.Dialer{Timeout: timeout, KeepAlive: 15 * time.Second}
	conn, err := d.Dial("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("连接 %s:%d 失败: %v", host, port, err)
	}
	cc, chans, reqs, err := ssh.NewClientConn(conn, addr, cfg)
	if err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("SSH 协商 %s:%d 失败: %v", host, port, err)
	}
	return &sshClient{addr: addr, client: ssh.NewClient(cc, chans, reqs)}, nil
}

func (c *sshClient) Close() error {
	if c.client == nil {
		return nil
	}
	err := c.client.Close()
	c.client = nil
	return err
}

// run executes one command with a wall-clock timeout and returns combined
// stdout+stderr. x/crypto v0.35 sessions have no context support, so the
// timeout is enforced by closing the session and the TCP connection.
func (c *sshClient) run(timeout time.Duration, command string) (string, error) {
	s, err := c.client.NewSession()
	if err != nil {
		return "", err
	}
	defer s.Close()
	var out bytes.Buffer
	s.Stdout = &out
	s.Stderr = &out
	if err := s.Start(command); err != nil {
		return "", err
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	waitCh := make(chan error, 1)
	go func() { waitCh <- s.Wait() }()
	select {
	case err := <-waitCh:
		tail := trimLine(out.String())
		if err != nil {
			return tail, fmt.Errorf("%v: %s", err, tail)
		}
		return tail, nil
	case <-timer.C:
		_ = s.Close()
		_ = c.Close()
		return trimLine(out.String()), fmt.Errorf("设备命令超时（%s）: %s", timeout.Round(time.Second), trimLine(out.String()))
	}
}

// writeFile sends data to the device with `umask 077; cat > path`. stdin
// transfer means the file content needs no shell escaping.
func (c *sshClient) writeFile(path string, data []byte, timeout time.Duration) error {
	s, err := c.client.NewSession()
	if err != nil {
		return err
	}
	defer s.Close()
	stdin, err := s.StdinPipe()
	if err != nil {
		return err
	}
	var errb bytes.Buffer
	s.Stderr = &errb
	if err := s.Start("umask 077; cat > " + shQuote(path)); err != nil {
		return err
	}
	werr := func() error {
		if _, e := stdin.Write(data); e != nil {
			return e
		}
		return stdin.Close()
	}()
	waitCh := make(chan error, 1)
	go func() { waitCh <- s.Wait() }()
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	var waitErr error
	select {
	case waitErr = <-waitCh:
	case <-timer.C:
		_ = s.Close()
		_ = c.Close()
		waitErr = fmt.Errorf("写入超时")
	}
	if werr != nil {
		return fmt.Errorf("写入 %s 失败: %v", path, werr)
	}
	if waitErr != nil {
		return fmt.Errorf("写入 %s 失败: %v: %s", path, waitErr, trimLine(errb.String()))
	}
	return nil
}

// shQuote single-quotes a value for the device ash shell.
func shQuote(s string) string {
	return "'" + string(bytes.ReplaceAll([]byte(s), []byte{'\''}, []byte(`'\''`))) + "'"
}

func trimLine(s string) string {
	b := []byte(s)
	for len(b) > 0 && (b[len(b)-1] == '\n' || b[len(b)-1] == '\r') {
		b = b[:len(b)-1]
	}
	return string(b)
}
