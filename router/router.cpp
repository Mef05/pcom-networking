extern "C" {
#include "lib.h"
#include "protocols.h"
#include "queue.h"
}

#include <arpa/inet.h>
#include <array>
#include <list>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct trie_node {
  struct trie_node *left;  // 0 bit
  struct trie_node *right; // 1 bit
  struct route_table_entry *entry;
};

struct trie_node *create_trie_node() {
  return reinterpret_cast<struct trie_node *>(
      calloc(1, sizeof(struct trie_node)));
}

void insert_route(struct trie_node *root, struct route_table_entry *route) {
  struct trie_node *curr = root;
  uint32_t prefix = ntohl(route->prefix);
  uint32_t mask = ntohl(route->mask);

  int bits = 0;
  for (int i = 31; i >= 0; i--) {
    if (mask & (1U << i)) {
      bits++;
    } else {
      break;
    }
  }

  for (int i = 31; i >= 32 - bits; i--) {
    int bit = (prefix >> i) & 1;
    if (bit == 0) {
      if (!curr->left)
        curr->left = create_trie_node();
      curr = curr->left;
    } else {
      if (!curr->right)
        curr->right = create_trie_node();
      curr = curr->right;
    }
  }

  curr->entry = route;
}

struct route_table_entry *get_best_route(struct trie_node *root, uint32_t ip) {
  struct trie_node *curr = root;
  struct route_table_entry *best = NULL;
  uint32_t host_ip = ntohl(ip);

  for (int i = 31; i >= 0; i--) {
    if (!curr)
      break;

    if (curr->entry) {
      best = curr->entry;
    }

    int bit = (host_ip >> i) & 1;
    if (bit == 0) {
      curr = curr->left;
    } else {
      curr = curr->right;
    }
  }

  if (curr && curr->entry) {
    best = curr->entry;
  }

  return best;
}

struct PendingPacket {
  std::vector<uint8_t> data;
  size_t len;
  size_t out_interface;
  uint32_t next_hop_ip;
};

void send_icmp(struct ether_hdr *old_eth, struct ip_hdr *old_ip, int interface,
               uint8_t type, uint8_t code) {
  char new_packet[MAX_PACKET_LEN];
  memset(new_packet, 0, MAX_PACKET_LEN);

  struct ether_hdr *new_eth = reinterpret_cast<struct ether_hdr *>(new_packet);
  struct ip_hdr *new_ip =
      reinterpret_cast<struct ip_hdr *>(new_packet + sizeof(struct ether_hdr));
  struct icmp_hdr *new_icmp = reinterpret_cast<struct icmp_hdr *>(
      new_packet + sizeof(struct ether_hdr) + sizeof(struct ip_hdr));

  // Original IP header + first 8 bytes of payload
  size_t payload_len = (old_ip->ihl * 4) + 8;
  size_t total_len = sizeof(struct ether_hdr) + sizeof(struct ip_hdr) +
                     sizeof(struct icmp_hdr) + payload_len;

  // 1. Ethernet Header
  memcpy(new_eth->ethr_dhost, old_eth->ethr_shost, 6);
  get_interface_mac(interface, new_eth->ethr_shost);
  new_eth->ethr_type = htons(0x0800);

  // 2. IP Header
  new_ip->ihl = 5;
  new_ip->ver = 4;
  new_ip->tos = 0;
  new_ip->tot_len =
      htons(sizeof(struct ip_hdr) + sizeof(struct icmp_hdr) + payload_len);
  new_ip->id = htons(1);
  new_ip->frag = 0;
  new_ip->ttl = 64;
  new_ip->proto = 1;
  new_ip->source_addr = inet_addr(get_interface_ip(interface));
  new_ip->dest_addr = old_ip->source_addr;

  new_ip->checksum = 0;
  new_ip->checksum = htons(
      checksum(reinterpret_cast<uint16_t *>(new_ip), sizeof(struct ip_hdr)));

  // 3. ICMP Header
  new_icmp->mtype = type;
  new_icmp->mcode = code;
  new_icmp->un_t.echo_t.id = 0;
  new_icmp->un_t.echo_t.seq = 0;

  // 4. Payload
  void *payload = new_packet + sizeof(struct ether_hdr) +
                  sizeof(struct ip_hdr) + sizeof(struct icmp_hdr);
  memcpy(payload, old_ip, payload_len);

  // 5. ICMP Checksum
  new_icmp->check = 0;
  new_icmp->check = htons(checksum(reinterpret_cast<uint16_t *>(new_icmp),
                                   sizeof(struct icmp_hdr) + payload_len));

  send_to_link(total_len, new_packet, interface);
}

int main(int argc, char *argv[]) {
  char buf[MAX_PACKET_LEN];

  // Do not modify this line
  init(argv + 2, argc - 2);

  std::vector<struct route_table_entry> table(100000);
  int entries = read_rtable(argv[1], table.data());
  table.resize(entries);

  // Build the Trie for Longest Prefix Match
  struct trie_node *root = create_trie_node();
  for (int i = 0; i < entries; i++) {
    insert_route(root, &table[i]);
  }

  // Cache router IPs (network byte order) to avoid repeated inet_addr calls in
  // hot path
  std::unordered_set<uint32_t> my_ips;
  for (int i = 0; i < ROUTER_NUM_INTERFACES; i++) {
    my_ips.insert(inet_addr(get_interface_ip(i)));
  }

  std::unordered_map<uint32_t, std::array<uint8_t, 6>> arp_table;
  std::list<PendingPacket> packet_queue;

  while (1) {
    size_t interface;
    size_t out_interface;
    size_t len;

    interface = recv_from_any_link(buf, &len);
    DIE(interface < 0, "recv_from_any_links");

    struct ether_hdr *eth = reinterpret_cast<struct ether_hdr *>(buf);

    if (ntohs(eth->ethr_type) == 0x0806) {
      struct arp_hdr *arp =
          reinterpret_cast<struct arp_hdr *>(buf + sizeof(struct ether_hdr));
      uint16_t opcode = ntohs(arp->opcode);

      if (opcode == 1) {
        // ARP Request - reply only if the target IP is ours
        if (my_ips.count(arp->tprotoa)) {
          memcpy(eth->ethr_dhost, eth->ethr_shost, 6);
          get_interface_mac(interface, eth->ethr_shost);

          arp->opcode = htons(2);
          memcpy(arp->thwa, arp->shwa, 6);
          get_interface_mac(interface, arp->shwa);

          arp->tprotoa = arp->sprotoa;
          arp->sprotoa = inet_addr(get_interface_ip(interface));

          send_to_link(len, buf, interface);
        }
      } else if (opcode == 2) {
        uint32_t sender_ip = ntohl(arp->sprotoa);
        std::array<uint8_t, 6> sender_mac;
        memcpy(sender_mac.data(), arp->shwa, 6);

        arp_table[sender_ip] = sender_mac;

        // Erase-while-iterating: no temp queue needed
        for (auto it = packet_queue.begin(); it != packet_queue.end();) {
          if (it->next_hop_ip == sender_ip) {
            struct ether_hdr *pkt_eth =
                reinterpret_cast<struct ether_hdr *>(it->data.data());
            memcpy(pkt_eth->ethr_dhost, sender_mac.data(), 6);
            get_interface_mac(it->out_interface, pkt_eth->ethr_shost);
            send_to_link(it->len, reinterpret_cast<char *>(it->data.data()),
                         it->out_interface);
            it = packet_queue.erase(it);
          } else {
            ++it;
          }
        }
      }
      continue;
    }

    if (ntohs(eth->ethr_type) == 0x0800) {
      struct ip_hdr *iphdr =
          reinterpret_cast<struct ip_hdr *>(buf + sizeof(struct ether_hdr));
      uint32_t dest = iphdr->dest_addr;

      // Checksum verification
      uint16_t old_checksum = iphdr->checksum;
      iphdr->checksum = 0;
      if (htons(checksum(reinterpret_cast<uint16_t *>(iphdr),
                         iphdr->ihl * 4)) != old_checksum) {
        continue;
      }
      iphdr->checksum = old_checksum;

      if (my_ips.count(dest)) {
        if (iphdr->proto == 1) {
          struct icmp_hdr *icmp_req = reinterpret_cast<struct icmp_hdr *>(
              buf + sizeof(struct ether_hdr) + (iphdr->ihl * 4));
          if (icmp_req->mtype == 8 && icmp_req->mcode == 0) {
            memcpy(eth->ethr_dhost, eth->ethr_shost, 6);
            get_interface_mac(interface, eth->ethr_shost);

            uint32_t tmp_ip = iphdr->source_addr;
            iphdr->source_addr = iphdr->dest_addr;
            iphdr->dest_addr = tmp_ip;

            iphdr->checksum = 0;
            iphdr->checksum = htons(
                checksum(reinterpret_cast<uint16_t *>(iphdr), iphdr->ihl * 4));

            icmp_req->mtype = 0;
            icmp_req->check = 0;
            size_t icmp_len = ntohs(iphdr->tot_len) - (iphdr->ihl * 4);
            icmp_req->check = htons(
                checksum(reinterpret_cast<uint16_t *>(icmp_req), icmp_len));

            send_to_link(len, buf, interface);
          }
        }
        continue;
      }

      struct route_table_entry *best_route = get_best_route(root, dest);

      if (!best_route) {
        // Destination Unreachable
        send_icmp(eth, iphdr, interface, 3, 0);
        continue;
      }

      if (iphdr->ttl <= 1) {
        // Time Exceeded
        send_icmp(eth, iphdr, interface, 11, 0);
        continue;
      }

      iphdr->ttl -= 1;
      iphdr->checksum = 0;
      iphdr->checksum =
          htons(checksum(reinterpret_cast<uint16_t *>(iphdr), iphdr->ihl * 4));

      uint32_t next_hop = ntohl(best_route->next_hop);
      out_interface = best_route->interface;

      // Single ARP table lookup with find (avoids double lookup of count+[])
      auto arp_it = arp_table.find(next_hop);
      if (arp_it != arp_table.end()) {
        uint8_t src_mac[6];
        get_interface_mac(out_interface, src_mac);
        memcpy(eth->ethr_dhost, arp_it->second.data(), 6);
        memcpy(eth->ethr_shost, src_mac, 6);
        send_to_link(len, buf, out_interface);
      } else {
        // Queue packet and send ARP Request (broadcast)
        PendingPacket p;
        p.data = std::vector<uint8_t>(buf, buf + len);
        p.len = len;
        p.out_interface = out_interface;
        p.next_hop_ip = next_hop;
        packet_queue.push_back(std::move(p));

        char arp_req_buf[MAX_PACKET_LEN];
        memset(arp_req_buf, 0, MAX_PACKET_LEN);

        struct ether_hdr *eth_req =
            reinterpret_cast<struct ether_hdr *>(arp_req_buf);
        struct arp_hdr *arp_req = reinterpret_cast<struct arp_hdr *>(
            arp_req_buf + sizeof(struct ether_hdr));

        memset(eth_req->ethr_dhost, 0xFF, 6);
        get_interface_mac(out_interface, eth_req->ethr_shost);
        eth_req->ethr_type = htons(0x0806);

        arp_req->hw_type = htons(1);
        arp_req->proto_type = htons(0x0800);
        arp_req->hw_len = 6;
        arp_req->proto_len = 4;
        arp_req->opcode = htons(1);

        get_interface_mac(out_interface, arp_req->shwa);
        arp_req->sprotoa = inet_addr(get_interface_ip(out_interface));

        memset(arp_req->thwa, 0x00, 6);
        arp_req->tprotoa = htonl(next_hop);

        send_to_link(sizeof(struct ether_hdr) + sizeof(struct arp_hdr),
                     arp_req_buf, out_interface);
      }
    }
  }
}
