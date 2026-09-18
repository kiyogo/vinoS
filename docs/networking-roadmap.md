# KOS Network and Security Stack

## Network Components

- Network Device Driver: Native Intel e1000 Gigabit Ethernet controller driver.
- Protocol Layer: Freestanding implementation of Ethernet II, ARP, IPv4, ICMP, DHCP, and DNS.
- Transport Layer: UDP and TCP socket subsystem supporting concurrent connections.
- Web Client and Server: HTTP and HTTPS client capabilities and embedded lightweight HTTP server.

## Cryptography and TLS

- TLS Record Layer: Freestanding TLS 1.3 encryption and key exchange.
- Certificate Verification: X509 certificate decoding and ASN1 parsing.
- Crypto Primitives: AES GCM encryption and SHA256 hashing.
