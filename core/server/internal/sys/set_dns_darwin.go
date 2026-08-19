package sys

import (
	tun "github.com/sagernet/sing-tun"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/common/shell"
	"strings"
	"sync"
)

type dnsState struct {
	service string
	servers []string
}

var (
	dnsStateMu sync.Mutex
	savedDNS   *dnsState
)

func SetSystemDNS(addr string, interfaceMonitor tun.DefaultInterfaceMonitor) error {
	dnsStateMu.Lock()
	defer dnsStateMu.Unlock()

	if addr == "Empty" {
		// "Empty" asks networksetup to discard manually configured DNS. That
		// is only right for a service which originally used DHCP; restore the
		// exact saved list for a user with custom resolvers instead.
		if savedDNS == nil {
			return nil
		}
		args := []string{"-setdnsservers", savedDNS.service}
		if len(savedDNS.servers) == 0 {
			args = append(args, "Empty")
		} else {
			args = append(args, savedDNS.servers...)
		}
		if err := runNetworkSetup(args...); err != nil {
			return err
		}
		savedDNS = nil
		return nil
	}

	interfaceName := interfaceMonitor.DefaultInterface().Name
	interfaceDisplayName, err := getInterfaceDisplayName(interfaceName)
	if err != nil {
		return err
	}

	if savedDNS == nil {
		servers, err := getSystemDNS(interfaceDisplayName)
		if err != nil {
			return err
		}
		savedDNS = &dnsState{service: interfaceDisplayName, servers: servers}
	}

	return runNetworkSetup("-setdnsservers", interfaceDisplayName, addr)
}

func runNetworkSetup(args ...string) error {
	// networksetup commits through configd; only its plist backup copy needs root, so its
	// "Permission denied" is noise unless the command itself failed.
	output, err := shell.Exec("/usr/sbin/networksetup", args...).Read()
	if err != nil {
		if output = strings.TrimSpace(output); output != "" {
			return E.Cause(err, output)
		}
		return err
	}

	return nil
}

func getSystemDNS(service string) ([]string, error) {
	output, err := shell.Exec("/usr/sbin/networksetup", "-getdnsservers", service).Read()
	if err != nil {
		if output = strings.TrimSpace(output); output != "" {
			return nil, E.Cause(err, output)
		}
		return nil, err
	}
	output = strings.TrimSpace(output)
	if output == "" || strings.Contains(output, "There aren't any DNS Servers set") {
		return nil, nil
	}
	return strings.Fields(output), nil
}

func getInterfaceDisplayName(name string) (string, error) {
	content, err := shell.Exec("/usr/sbin/networksetup", "-listallhardwareports").ReadOutput()
	if err != nil {
		return "", err
	}
	for _, deviceSpan := range strings.Split(string(content), "Ethernet Address") {
		if strings.Contains(deviceSpan, "Device: "+name) {
			substr := "Hardware Port: "
			deviceSpan = deviceSpan[strings.Index(deviceSpan, substr)+len(substr):]
			deviceSpan = deviceSpan[:strings.Index(deviceSpan, "\n")]
			return deviceSpan, nil
		}
	}
	return "", E.New(name, " not found in networksetup -listallhardwareports")
}
