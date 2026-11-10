# PCom networking projects

Two homework projects from the Communication Protocols (PCom) course at
University Politehnica of Bucharest, spring 2026.

| Folder | Project |
|---|---|
| [`router/`](router/) | IPv4 router dataplane in C++: forwarding with checksum and TTL handling, longest-prefix match on a binary trie, ARP, and ICMP echo reply, destination unreachable and time exceeded. |
| [`transport/`](transport/) | POLI-TCP, a reliable transport over UDP: handshake that moves each connection to its own port, Selective Repeat sliding window, SRTT/RTO estimation and per-segment retransmission driven by timerfd and poll. |

Each folder has its own README (in Romanian) describing the implementation.

## Credits

Each project starts from the course team's public starter code
(checker, topology, helper libraries), imported in its own commit:

- router: https://gitlab.cs.pub.ro/pcom/homework1-public
- transport: https://gitlab.cs.pub.ro/pcom/homework3-public

All later commits are my own work.
