/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPDX-FileCopyrightText: 2024 - 1984 Hosting Company <1984@1984.is> */
/* SPDX-FileCopyrightText: 2024 - Freyx Solutions <frey@freyx.com> */
/* SPDX-FileContributor: Freysteinn Alfredsson <freysteinn@freysteinn.com> */
/* SPDX-FileContributor: Julius Thor Bess Rikardsson <juliusbess@gmail.com> */

/**
 * @file neighsnoopd.c
 * @brief The primary file for decision-making and handling.
 *
 * This is the primary file for the neighsnoopd program and handles the core
 * functionality and decision-making. The core part of the program is the epoll
 * loop, which monitors all events such as POSIX signals, Netlink messages, the
 * eBPF ring buffer for ARP/ND data extracted by the XDP/TC eBPF code, and
 * client requests for statistics. The neighsnoopd program is designed to be
 * single-threaded, relying on file descriptors to manage all the events it
 * needs to handle.
 *
 * @see neighsnoopd.h for the main header data structures and functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <argp.h>
#include <time.h>
#include <ifaddrs.h>
#include <regex.h>
#include <string.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <linux/bpf.h>
#include <linux/rtnetlink.h>
#include <linux/ipv6.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <net/ethernet.h>
#include <net/if_arp.h>

#include <netinet/if_ether.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

#include "neighsnoopd.h"

#include "neighsnoopd_shared.h" // Shared struct neighbor_reply with BPF
#include "neighsnoopd.bpf.skel.h"

#include "version.in.h"

struct env env = {0};

const char *argp_program_version = "neighsnoopd v0.9\n"
    "Build date: " __DATE__ " " __TIME__ "\n" \
    "git commit: " GIT_COMMIT;

const char *argp_program_bug_address =
        "https://github.com/1984hosting/neighsnoopd";

const char argp_program_doc[] =
    "Listens for ARP and NA replies and adds the neighbor to the Neighbors"
    "table.\n";

static const struct argp_option opts[] = {
    { "ipv4", '4', NULL, 0, "Only handle IPv4 ARP Reply packets", 0 },
    { "ipv6", '6', NULL, 0, "Only handle IPv6 NA packets", 0 },
    { "count", 'c', "NUM", 0, "This option handles a fixed number of ARP or NA"
      "replies before terminating the program."
      "Use this for debugging purposes only", 0 },
    { "deny-filter", 'f', "REGEXP", 0,
      "Filters out interfaces with a regular expression exclude from adding to"
      "the neighbor cache. Example: -f '^br0|.*-v1$'", 0 },
    { "disable_ipv6ll_filter", 'l', NULL, 0,
      "Disable the default IPv6 link-local filter", 0},
    { "no-qfilter-present", 'q', NULL, 0, "Do not replace the present Qdisc"
      "filter if it is present on the Ingress device", 0 },
    { "verbose", 'v', NULL, 0, "Verbose debug output", 0 },
    { "xdp", 'x', NULL, 0, "Attach XDP instead of TC. This option only works"
      "on devices with a VLAN header on the packets available to XDP.", 0},
    { NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help", 0 },
    {},
};

static bool filter_deny_interfaces(char *ifname);
static int new_neigh_timer(struct neigh_cache *neigh);

// Callback function to handle data from the BPF ring buffer
static int handle_neighbor_reply(void *ctx, void *data, size_t data_sz)
{
    struct neighbor_reply *neighbor_reply = (struct neighbor_reply *)data;
    struct link_network_cache *link_net;
    struct link_cache *link;
    struct fdb_cache *fdb;
    struct neigh_cache *neigh;
    __u8 mac_str[MAC_ADDR_STR_LEN];
    char ip_str[INET6_ADDRSTRLEN];

    if (!neighbor_reply) {
        pr_err(0, "Neighbor Reply: Invalid data");
        return 1;
    }

    if (env.only_ipv6 && neighbor_reply->in_family != AF_INET6)
        return 1;
    else if (env.only_ipv4 && neighbor_reply->in_family != AF_INET)
        return 1;

    env.count--;

    link_net = cache_get_link_network_by_reply(neighbor_reply);
    if (!link_net) {
        pr_err(0, "NIC with VLAN ID: %d Network: %d not found in cache",
               neighbor_reply->vlan_id, neighbor_reply->network_id);
        return 1;
    }
    link = link_net->link;

    fdb = cache_get_fdb_by_reply(neighbor_reply, link->ifindex);
    if (fdb) {
        pr_debug("Neighbor Reply: IP: %s MAC: %s nic: %s is externally learned. Skipping\n",
                 fdb->mac_str, link_net->network->network_str, fdb->link->ifname);
        return 0;
    }

    mac_to_string(mac_str, neighbor_reply->mac, sizeof(mac_str));
    format_ip_address(ip_str, sizeof(ip_str), &neighbor_reply->ip);
    pr_debug("Neighbor Reply: IP: %s MAC: %s nic: %s\n",
            ip_str, mac_str, link->ifname);

    neigh = cache_get_neigh_by_reply(neighbor_reply, link->ifindex);
    if (neigh) {

        // Remove old timer
        if (neigh->timer)
            timer_remove_event((union timer_cmd *)neigh->timer);
        // Add a new timer
        if (new_neigh_timer(neigh))
            return 1;
    }

    // Make the neighbor entry reachable in the Linux kernel's neighbor table
    // This will also prompt a Netlink reply from the kernel that we will use to
    // update our local cache
    netlink_send_neigh(neighbor_reply, link->ifindex);

    return 0;
}

// Function to calculate the checksum for an IPv6 pseudo-header and payload
static uint16_t checksum(const void *data, int len)
{
    uint32_t sum = 0;
    const uint16_t *ptr = data;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(uint8_t *)ptr;
    }

    // Fold 32-bit sum to 16 bits
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    return (uint16_t)~sum;
}

static int send_neighbor_solicitation(struct neigh_cache *neigh)
{
    struct link_network_cache *src = neigh->sending_link_network;
    unsigned char buffer[ETH_HLEN + sizeof(struct ipv6hdr)
                         + sizeof(struct nd_neighbor_solicit) + 8];

    // Zero out the buffer
    memset(buffer, 0, sizeof(buffer));

    // Ethernet header
    struct ethhdr *eth = (struct ethhdr *)buffer;
    memcpy(eth->h_dest, neigh->mac, ETH_ALEN);       // Target MAC address
    memcpy(eth->h_source, src->link->mac, ETH_ALEN); // Source MAC address
    eth->h_proto = htons(ETH_P_IPV6);                // IPv6 EtherType

    // IPv6 header
    struct ipv6hdr *ip6 = (struct ipv6hdr *)(buffer + ETH_HLEN);
    ip6->version = 6;
    ip6->priority = 0;
    memset(ip6->flow_lbl, 0, sizeof(ip6->flow_lbl));
    ip6->payload_len = htons(sizeof(struct nd_neighbor_solicit) + 8);
    ip6->nexthdr = IPPROTO_ICMPV6;
    ip6->hop_limit = 255; // Required for NS messages
    memcpy(&ip6->saddr, &src->ip, sizeof(struct in6_addr));   // Source IPv6
    memcpy(&ip6->daddr, &neigh->ip, sizeof(struct in6_addr)); // Target IPv6

    // ICMPv6 Neighbor Solicitation (after IPv6 header)
    struct nd_neighbor_solicit *ns = (struct nd_neighbor_solicit *)
                                     (buffer + ETH_HLEN + sizeof(struct ipv6hdr));
    ns->nd_ns_type = ND_NEIGHBOR_SOLICIT;
    ns->nd_ns_code = 0;
    ns->nd_ns_cksum = 0; // Will calculate the checksum below
    ns->nd_ns_reserved = 0;
    memcpy(&ns->nd_ns_target, &neigh->ip, sizeof(struct in6_addr));

    // ICMPv6 option - Source Link-Layer Address (after NS)
    unsigned char *opt = (unsigned char *)(buffer + ETH_HLEN
                                           + sizeof(struct ipv6hdr)
                                           + sizeof(struct nd_neighbor_solicit));
    opt[0] = 1; // Option type: Source Link-Layer Address
    opt[1] = 1; // Option length in units of 8 octets
    memcpy(opt + 2, src->link->mac, ETH_ALEN); // Source MAC address

    // Pseudo-header for checksum calculation
    struct {
        struct in6_addr src;
        struct in6_addr dst;
        uint32_t length;
        uint8_t zeros[3];
        uint8_t next_header;
    } pseudo_header;

    memset(&pseudo_header, 0, sizeof(pseudo_header));
    memcpy(&pseudo_header.src, &ip6->saddr, sizeof(struct in6_addr));
    memcpy(&pseudo_header.dst, &ip6->daddr, sizeof(struct in6_addr));
    pseudo_header.length = htonl(sizeof(struct nd_neighbor_solicit) + 8);
    pseudo_header.next_header = IPPROTO_ICMPV6;

    // Calculate the checksum
    uint8_t pseudo_buffer[sizeof(pseudo_header) +
                          sizeof(struct nd_neighbor_solicit) + 8];
    memcpy(pseudo_buffer, &pseudo_header, sizeof(pseudo_header));
    memcpy(pseudo_buffer + sizeof(pseudo_header), ns,
           sizeof(struct nd_neighbor_solicit) + 8);

    ns->nd_ns_cksum = checksum(pseudo_buffer, sizeof(pseudo_buffer));

    // Set up the destination address for sending
    struct sockaddr_ll dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sll_family = AF_PACKET;
    dest_addr.sll_protocol = htons(ETH_P_IPV6);
    dest_addr.sll_halen = ETH_ALEN;
    memcpy(dest_addr.sll_addr, neigh->mac, ETH_ALEN);
    dest_addr.sll_ifindex = src->link->ifindex;

    // Send the Neighbor Solicitation message
    if (sendto(env.packet_fd, buffer, sizeof(buffer), 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        pr_err(errno, "Neighbor Solicitation send failed");
        return -1;
    }

    pr_debug("Neighbor Solicitation (NS) sent to IP: %s from nic: %s\n",
             neigh->ip_str, src->link->ifname);
    return 0;
}

static int send_arp_request(struct neigh_cache *neigh)
{
    struct link_network_cache *src = neigh->sending_link_network;
    unsigned char buffer[ETH_HLEN + sizeof(struct ether_arp)];

    in_addr_t src_ip = src->ip.s6_addr32[3];
    in_addr_t dst_ip = neigh->ip.s6_addr32[3];

    // Zero out the buffer
    memset(buffer, 0, sizeof(buffer));

    // Ethernet header
    struct ethhdr *eth = (struct ethhdr *)buffer;
    memcpy(eth->h_dest, neigh->mac, 6);       // Target MAC address
    memcpy(eth->h_source, src->link->mac, 6); // Source MAC address
    eth->h_proto = htons(ETH_P_ARP);          // ARP protocol

    // ARP header
    struct ether_arp *arp = (struct ether_arp *)(buffer + ETH_HLEN);
    arp->ea_hdr.ar_hrd = htons(ARPHRD_ETHER);    // Hardware type (Ethernet)
    arp->ea_hdr.ar_pro = htons(ETH_P_IP);        // Protocol type (IPv4)
    arp->ea_hdr.ar_hln = ETH_ALEN;               // Hardware address length
    arp->ea_hdr.ar_pln = sizeof(struct in_addr); // Protocol address length
    arp->ea_hdr.ar_op = htons(ARPOP_REQUEST);    // ARP operation (request)

    // Fill ARP request details
    memcpy(arp->arp_sha, src->link->mac, ETH_ALEN); // Sender MAC address
    memcpy(arp->arp_spa, &src_ip, sizeof(src_ip));  // Sender IP address
    memset(arp->arp_tha, 0, ETH_ALEN);              // Target MAC address (unknown)
    memcpy(arp->arp_tpa, &dst_ip, sizeof(dst_ip));  // Target IP address

    // Set up the destination address for sending
    struct sockaddr_ll dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sll_family = AF_PACKET;
    dest_addr.sll_protocol = htons(ETH_P_ARP);
    dest_addr.sll_halen = ETH_ALEN;
    memcpy(dest_addr.sll_addr, neigh->mac, 6);
    dest_addr.sll_ifindex = src->link->ifindex;

    // Send the ARP request using the existing socket env.packet_fd
    if (sendto(env.packet_fd, buffer, sizeof(buffer), 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        pr_err(errno, "ARP request send failed");
        return -1;
    }

    pr_debug("Gratuitous ARP request sent to IP: %s from nic: %s\n",
             neigh->ip_str, src->link->ifname);

    return 0;
}

static void send_gratuitous_neighbor_request(struct neigh_cache *neigh)
{
    if (IN6_IS_ADDR_V4MAPPED(&neigh->ip))
        send_arp_request(neigh);
    else
        send_neighbor_solicitation(neigh);
}

int handle_timer_neigh_event(struct timer_neigh_cmd *cmd)
{
    send_gratuitous_neighbor_request(cmd->neigh);
    cmd->neigh->timer = NULL; // Remove the timer reference
    return 0;
}

int handle_timer_event(union timer_cmd *cmd)
{
    int ret = -1;
    switch (cmd->base.type) {
        case TIMER_NEIGH:
            ret = handle_timer_neigh_event(&cmd->neigh);
            break;
        default:
            pr_err(0, "Unknown timer event\n");
            break;
    }
    return ret;
}

static int get_next_gratuitous_time(struct neigh_cache *neigh,
                                       double *seconds)
{
    int ret = -1;
    struct link_cache *link = neigh->sending_link_network->link;
    double base_reachable_time;
    char path[PATH_MAX];
    bool is_ipv4 = IN6_IS_ADDR_V4MAPPED(&neigh->ip);
    FILE *fp;

    snprintf(path, sizeof(path),
             "/proc/sys/net/%s/neigh/%s/base_reachable_time_ms",
             is_ipv4 ? "ipv4" : "ipv6",
             link->ifname);

    fp = fopen(path, "r");
    if (!fp) {
        pr_err(errno, "Failed to open %s", path);
        goto out0;
    }

    if (fscanf(fp, "%lf", &base_reachable_time) != 1) {
        pr_err(errno, "Failed to read %s", path);
        goto out1;
    }

    /*
     * Our primary aim is to send gratuitous neighbor requests before the
     * kernel changes the nud state to STALE. Therefore, we will change the
     * time to one-fourth of the base_reachable_time and add a random time of
     * up to two seconds to prevent too many gratuitous requests from happening
     * simultaneously. This is an arbitrary choice for the time.
     */
    *seconds = base_reachable_time / 4.0 / 1000.0 +
        (rand() % 2000) / 1000.0;

    ret = 0;

out1:
    fclose(fp);
out0:
    return ret;
}

static int new_neigh_timer(struct neigh_cache *neigh)
{
    double next_gratuitous_time;
    if (get_next_gratuitous_time(neigh, &next_gratuitous_time))
        return -1;

    if (timer_add_neigh(neigh, next_gratuitous_time)) {
        pr_err(0, "Failed to add timer for Neigh: IP: %s MAC: %s\n",
               neigh->ip_str, neigh->mac_str);
        return -1;
    }

    pr_debug("Neigh: IP: %s MAC: %s nic: %s added timer for %f seconds\n",
             neigh->ip_str, neigh->mac_str,
             neigh->sending_link_network->link->ifname,
             next_gratuitous_time);

    return 0;

}

static int handle_neigh_add(struct netlink_neigh_cmd *cmd)
{
    struct link_cache *link;
    struct link_network_cache *link_net;
    struct neigh_cache *neigh;

    char ip_str[INET6_ADDRSTRLEN];
    __u8 mac_str[MAC_ADDR_STR_LEN];

    // Ignore neigh events until we are initialized
    if (!(env.has_links && env.has_networks && env.has_fdb))
        goto out;

    if (env.debug) {
        format_ip_address(ip_str, sizeof(ip_str), &cmd->ip);
        mac_to_string(mac_str, cmd->mac, sizeof(mac_str));
    }

    // Skip entries without an interface
    if (cmd->ifindex == 0) {
        pr_debug("Neigh: IP: %s MAC: %s has no interface\n",
                 ip_str, mac_str);
        goto out;
    }

    // Skip incomplete entries
    if (is_zero_mac(cmd->mac))
        goto out;

    // We skip externally learned entries
    if (cmd->is_externally_learned) {
        pr_debug("Neigh: IP: %s MAC: %s is externally learned\n",
                 ip_str, mac_str);
        goto out;
    }

    // Check if the link is a connected SVI
    link = cache_get_link(cmd->ifindex);
    if (!link) {
        pr_err(0, "Failed to lookup interface %d", cmd->ifindex);
        goto out;
    }

    // Ignoring IPs that are not part of a target network
    link_net = cache_get_link_network_by_addr(link, &cmd->ip);
    if (!link_net)
        goto out;

    neigh = cache_get_neigh(cmd);
    if (neigh) { // Already cached
        cache_neigh_update(cmd);
    } else { // Create a new cache entry
        neigh = cache_add_neigh(link_net, cmd);
        if (!neigh) {
            pr_err(0, "Failed to add Neigh: IP: %s MAC: %s to cache\n",
                   ip_str, mac_str);
            goto out;
        }
        pr_info("Neigh: IP: %s MAC: %s nic: %s added to cache\n",
                neigh->ip_str, neigh->mac_str,
                link->ifname);
    }

    // Add a timer to send a gratuitous neighbor request
    if (neigh->nud_state == NUD_REACHABLE && !neigh->timer) {
        if (new_neigh_timer(neigh))
            goto out;
    } else if (neigh->nud_state == NUD_REACHABLE)
        pr_debug("Neigh: IP: %s MAC: %s nic: %s already has a timer\n",
                 neigh->ip_str, neigh->mac_str, link->ifname);

    // Send a Link layer address resolution request to check if the
    // neighbor is still there
    if (neigh->nud_state == NUD_STALE)
        send_gratuitous_neighbor_request(neigh);

out:
    return 0;
}

static int handle_neigh_del(struct netlink_neigh_cmd *cmd)
{
    struct neigh_cache *neigh = cache_get_neigh(cmd);

    if (!neigh) // Not cached
        goto out;

    if (neigh->timer) {
        timer_remove_event((union timer_cmd *)neigh->timer);
        neigh->timer = NULL;
    }

    cache_del_neigh(neigh);

out:
    return 0;
}

static int handle_fdb_add(struct netlink_neigh_cmd *cmd)
{
    int ret = 0;
    struct link_cache *link;
    struct fdb_cache *fdb;
    __u8 mac_str[MAC_ADDR_STR_LEN];

    // Ignore neigh events until we are initialized
    if (!(env.has_links && env.has_networks))
        goto out;

    // Skip entries without an interface
    if (cmd->ifindex == 0)
        goto out;

    link = cache_get_link(cmd->ifindex);
    if (!link) {
        pr_err(0, "Failed to lookup interface %d", cmd->ifindex);
        goto out;
    }

    if (cmd->is_externally_learned) {
        mac_to_string(mac_str, cmd->mac, sizeof(mac_str));
        pr_debug("FDB: MAC: %s is externally learned: Not cached\n", mac_str);
        goto out;
    }

    fdb = cache_get_fdb(cmd);
    if (fdb) // Already cached
        goto out;

    fdb = cache_add_fdb(cmd);
    if (!fdb) {
        mac_to_string(mac_str, cmd->mac, sizeof(mac_str));
        pr_err(0, "Failed to add FDB: MAC: %s to cache\n", mac_str);
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int handle_fdb_del(struct netlink_neigh_cmd *cmd)
{
    struct fdb_cache *fdb = cache_get_fdb(cmd);

    if (!fdb) // Not cached
        goto out;

    cache_del_fdb(cmd);

out:
    return 0;
}

static int handle_addr_add(struct netlink_addr_cmd *cmd)
{
    int ret = 0;
    struct network_cache *network;
    struct link_cache *link;
    struct link_network_cache *link_net;
    char network_cidr_str[INET6_ADDRSTRLEN + 4]; // IPv6 address + / + prefixlen

    // Ignore neigh events until we are initialized
    if (!env.has_links)
        goto out;

    if (!env.disable_ipv6ll_filter) {
        if (IN6_IS_ADDR_LINKLOCAL(&cmd->ip))
            goto out;
    }

    link = cache_get_link(cmd->ifindex);
    if (!link) {
        pr_debug("Failed to lookup interface %d\n", cmd->ifindex);
        goto out;
    }

    if (!link->is_svi) {
        pr_debug("Link: %s is not an SVI connected to the bridge\n",
                 link->ifname);
        goto out;
    }

    format_ip_address_cidr(network_cidr_str, sizeof(network_cidr_str),
                        &cmd->ip, cmd->prefixlen);

    network = cache_get_network(cmd);
    if (!network) {
        network = cache_add_network(cmd);
        if (!network) {
            pr_err(0, "Failed to add network %s to cache", network_cidr_str);
            ret = -1;
            goto out;
        }
    }

    link_net = cache_get_link_network(link->ifindex, network->network);
    if (!link_net) {
        link_net = cache_new_link_network();
        if (!link_net) {
            pr_err(0, "Failed to add link %s to network %s",
                   link->ifname, network_cidr_str);
            ret = -1;
            goto out;
        }

        link_net->link = link;
        link_net->network = network;
        link_net->ip = calculate_network_using_cidr(&network->network,
                                                    cmd->prefixlen);

        ret = cache_add_link_network(link_net);
        if (ret)
            goto out;

        pr_info("Cache: Added: Network(%d): %s with link %s\n",
                network->id, network_cidr_str, link->ifname);
    }

out:
    return ret;
}

static int handle_addr_del(struct netlink_addr_cmd *cmd)
{
    struct network_cache *network = cache_get_network(cmd);
    char network_cidr_str[INET6_ADDRSTRLEN + 4]; // IPv6 address + / + prefixlen

    if (!network) {
        format_ip_address_cidr(network_cidr_str, sizeof(network_cidr_str),
                            &cmd->ip, cmd->prefixlen);
        pr_debug("Network: %s not cached: Can't remove\n",
                 network_cidr_str);
        goto out;
    }

    cache_del_network(cmd);

    pr_info("Cache: Removing Network: %s/%d\n", network->network_str,
            network->prefixlen);

out:
    return 0;
}

static int handle_link_add(struct netlink_link_cmd *cmd)
{
    int ret = 0;
    struct link_cache *link;

    link = cache_get_link(cmd->ifindex);
    if (link) {
        pr_debug("Link: %d: %s already cached\n",
                 cmd->ifindex, cmd->ifname);
        cache_update_link(link, cmd);
        goto out;
    } else {
        link = cache_add_link(cmd);
        if (!link) {
            pr_err(errno, "Failed to add link %d: %s to cache",
                   cmd->ifindex, cmd->ifname);
            ret = -1;
            goto out;
        }
    }

    link->is_svi = cmd->link_ifindex == env.ifidx_mon ? true : false;

    if (filter_deny_interfaces(cmd->ifname)) {
        pr_debug("Link: %d: %s matches regexp filter: filtered\n",
                 cmd->ifindex, cmd->ifname);
        link->ignore_link = true;
    }

    if (link->is_svi)
        pr_info("Cache: Added: NIC: %s with vlan: %d\n",
            cmd->ifname, cmd->vlan_id);
    else
        pr_debug("Cache: Added: NIC: %s with vlan: %d\n",
            cmd->ifname, cmd->vlan_id);

out:
    return ret;
}

static int handle_link_del(struct netlink_link_cmd *cmd)
{
    struct link_cache *link = cache_get_link(cmd->ifindex);

    if (!link) {
        pr_debug("Cache: Link: %s not cached: Can't remove\n", cmd->ifname);
        goto out;
    }

    cache_del_link(cmd);

    pr_info("Cache: Link: Removed: %s\n", cmd->ifname);

out:
    return 0;
}

static int handle_netlink_cmd(union netlink_cmd *cmd)
{
    int ret = 0;
    switch (cmd->cmd_type) {
        case CMD_NEIGH_ADD:
            ret = handle_neigh_add(&cmd->neigh);
            break;
        case CMD_NEIGH_DEL:
            ret = handle_neigh_del(&cmd->neigh);
            break;
        case CMD_FDB_ADD:
            ret = handle_fdb_add(&cmd->neigh);
            break;
        case CMD_FDB_DEL:
            ret = handle_fdb_del(&cmd->neigh);
            break;
        case CMD_ADDR_ADD:
            ret = handle_addr_add(&cmd->addr);
            break;
        case CMD_ADDR_DEL:
            ret = handle_addr_del(&cmd->addr);
            break;
        case CMD_LINK_ADD:
            ret = handle_link_add(&cmd->link);
            break;
        case CMD_LINK_DEL:
            ret = handle_link_del(&cmd->link);
            break;
        default:
            pr_err(0, "Unknown command\n");
            break;
    }

    netlink_cmd_free(cmd);

    return ret;
}

static int handle_netlink(void)
{
    union netlink_cmd *cmd;

    // Process all Netlink messages and prep the cmd queue
    netlink_process_rx_queue();

    while ((cmd = netlink_dequeue_cmd()))
        if (handle_netlink_cmd(cmd))
            return -1;

    return 0;
}

static int handle_signal(void)
{
    int err = 0;
    struct signalfd_siginfo fdsi;
    ssize_t s;

    s = read(env.signal_fd, &fdsi, sizeof(struct signalfd_siginfo));
    if (s != sizeof(struct signalfd_siginfo)) {
        pr_err(errno, "read");
        err = errno;
        goto out;
    }

    if (fdsi.ssi_signo == SIGINT || fdsi.ssi_signo == SIGTERM) {
        err = 1;
    }

out:
    return err;
}

static int handle_ring_buffer(void)
{
    int err;

    err = ring_buffer__consume(env.ringbuf);
    if (err < 0) {
        pr_err(err, "bpf_ringbuf_consume");
        goto out;
    }
    err = 0; // The return value is the number of consumed records

out:
    return err;
}

static void main_loop(void)
{
    struct epoll_event events[env.number_of_fds];
    struct epoll_event event;
    int client_offset;
    int client_bytes_to_send;
    bool last_round = false;

    if (netlink_queue_send_next()) {
        pr_err(errno, "Failed to send Netlink message");
        return; // Failure
    }

    while (true) {
        int n;

        if (env.has_count) {
            if (last_round)
                break;
            if (env.count <= 0)
                last_round = true;
        }

        n = epoll_wait(env.epoll_fd, events, env.number_of_fds, -1);
        if (n == -1) {
            if (errno == EINTR)
                continue; // Ignore interrupted by signal
            pr_err(errno, "epoll_wait");
            return; // Failure
        }

        /*
         * We priorities the events from epoll as follows:
         * 1. Signal events
         * 2. Handle timer events
         * 3. Netlink events
         * 4. BPF ring buffer events
         * 5. Send Netlink messages from the tx queue
         * 6. Handle stats server socket requests
         * 7. Handle stats client socket
         */

        // Signal events
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == env.signal_fd) {
                if (handle_signal())
                    return; // Failure or exiting
            }
        }

        // Handle timer events
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == env.timerfd_fd) {
                if (handle_timer_events()) {
                    pr_err(0, "Failed to process timer events");
                    return; // Failure
                }
            }
        }

        // Netlink events
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == env.nl_fd) {
                if (handle_netlink())
                    return; // Failure
            }
        }

        // BPF ring buffer events
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == env.ringbuf_fd) {
                if (handle_ring_buffer()) {
                    pr_err(errno, "Failed to consume ring buffer");
                    return; // Failure
                }
            }
        }

        // Send Netlink messages from the tx queue
        if (netlink_queue_send_next()) {
            pr_err(errno, "Failed to send Netlink message");
            return; // Failure
        }

        // Handle server stats request
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == env.stats_server_fd) {
                struct stat st;
                if (handle_stats_server_request())
                    return; // Failure

                // Add the client socket to the epoll
                event.events = EPOLLOUT | EPOLLRDHUP;
                event.data.fd = env.stats_client_fd;
                if (epoll_ctl(env.epoll_fd, EPOLL_CTL_ADD, env.stats_client_fd,
                              &event) == -1) {
                    pr_err(errno, "epoll_ctl: stats_client_fd");
                    close(env.stats_client_fd);
                    env.stats_client_fd = -1;
                    close(env.memfd_fd);
                    env.memfd_fd = -1;
                    continue;
                }
                client_offset = 0;
                if (fstat(env.memfd_fd, &st) == -1) {
                    pr_err(errno, "fstat");
                    return; // Failure
                }
                client_bytes_to_send = st.st_size;
            }
        }

        // Handle client stats request
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == env.stats_client_fd) {
                int bytes_read;
                int bytes_sent;
                char buf[4096];

                // Check if the client socket has been closed or if an error
                // occurred. Or if the client has received all the data
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP) ||
                    client_offset == client_bytes_to_send) {
                    // Clean up resources
                    close(env.stats_client_fd);
                    env.stats_client_fd = -1;
                    close(env.memfd_fd);
                    env.memfd_fd = -1;
                    continue;
                }

                bytes_read = pread(env.memfd_fd, buf, sizeof(buf),
                                   client_offset);
                if (bytes_read == -1) {
                    pr_err(errno, "pread");
                    return; // Failure
                }

                bytes_sent = send(env.stats_client_fd, buf, bytes_read, 0);
                if (bytes_sent == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        continue;
                    pr_err(errno, "send");
                    return; // Failure
                }

                client_offset += bytes_sent;

                if (client_offset == client_bytes_to_send) {
                    // Remove the client socket from the epoll
                    event.events = EPOLLOUT;
                    event.data.fd = env.stats_client_fd;
                    if (epoll_ctl(env.epoll_fd, EPOLL_CTL_DEL, env.stats_client_fd,
                                  &event) == -1) {
                        pr_err(errno, "epoll_ctl: stats_client_fd");
                        return; // Failure
                    }

                    // Close the client socket and the memfd
                    close(env.stats_client_fd);
                    env.stats_client_fd = -1;
                    close(env.memfd_fd);
                    env.memfd_fd = -1;
                }
            }
        }
    }
}

static bool filter_deny_interfaces(char *ifname)
{
    int ret;
    if (!env.has_deny_filter)
        return false;

    ret = regexec(&env.deny_filter, ifname, 0, NULL, 0);
    if (ret)
        return false;

    return true;
}

// Signal setup and cleanup
static int setup_signals(void)
{
    int err = 0;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);  // Handle SIGINT (Ctrl+C)
    sigaddset(&mask, SIGTERM); // Handle SIGTERM

    // Block these signals so they can be handled via signalfd
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        perror("sigprocmask");
        err = errno;
        goto out;
    }

    // Create a signalfd to receive the signals
    env.signal_fd = signalfd(-1, &mask, 0);
    if (env.signal_fd == -1) {
        perror("signalfd");
        err = errno;
        goto out;
    }

    env.number_of_fds++;

out:
    return err;
}

static void cleanup_signals(void)
{
    if (env.signal_fd >= 0)
        close(env.signal_fd);
}

// BPF setup and cleanup
static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
                           va_list args)
{
    if (level == LIBBPF_DEBUG && !env.debug)
        return 0;
    return vfprintf(stderr, format, args);
}

static int setup_bpf(void)
{
    int err = 0;

    libbpf_set_print(libbpf_print_fn);

    // Open the skeleton
    env.skel = neighsnoopd_bpf__open();
    if (!env.skel) {
        perror("Failed to open BPF skeleton\n");
        err = errno;
        env.skel = NULL;
        goto out;
    }

    // Load the BPF program
    err = neighsnoopd_bpf__load(env.skel);
    if (err) {
        perror("Failed to load BPF skeleton\n");
        err = errno;
        goto out;
    }

    env.target_networks_fd = bpf_map__fd(
        bpf_object__find_map_by_name(env.skel->obj, "target_networks"));

    // XDP
    struct bpf_link *xdp_link;

    // TC OPTS
    LIBBPF_OPTS(bpf_tc_hook, tc_hook,
                .ifindex = env.ifidx_mon,
                .attach_point = BPF_TC_INGRESS);
    LIBBPF_OPTS(bpf_tc_opts, tc_opts,
                .handle = 1,
                .priority = 1,
                .prog_fd = bpf_program__fd(
                    env.skel->progs.handle_neighbor_reply_tc));

    env.tc_opts = tc_opts;
    env.tc_hook = tc_hook;

    if (!env.fail_on_qfilter_present)
        env.tc_opts.flags |= BPF_TC_F_REPLACE;

    if (env.is_xdp) {
        // attach xdp program to interface
        xdp_link = bpf_program__attach_xdp(
            env.skel->progs.handle_neighbor_reply_xdp, env.ifidx_mon);
        if (!xdp_link) {
            perror("Failed to attach XDP hook");
            goto out;
        }
    } else {
        // Load TC hook instead of XDP
        // Attach the BPF program to the clsact qdisc for ingress

        /*
         * The TC Qdisc hook may already exist because:
         * 1. Other processes or users create it.
         * 2. By attaching to the TC ingress, the bpf_tc_hook_destroy does not
         * remove the Qdisc and may leave an egress filter in place since the last
         * invocation of the program.
         */
        err = bpf_tc_hook_create(&env.tc_hook);
        if (err && err != -EEXIST) {
            perror("Failed to create TC hook");
            goto out;
        }

        if (bpf_tc_attach(&env.tc_hook, &env.tc_opts)) {
            perror("Failed to attach TC hook");
            goto out;
        }
    }
    err = 0;

    // Parse Neighbor replies
    struct bpf_map *ringbuf_map =
        bpf_object__find_map_by_name(env.skel->obj, "neighbor_ringbuf");

    env.ringbuf = ring_buffer__new(bpf_map__fd(ringbuf_map),
                                              handle_neighbor_reply, NULL, NULL);
    if (!env.ringbuf) {
        perror("Failed to create ring buffer");
        err = errno;
        goto out;
    }

    env.ringbuf_fd = bpf_map__fd(ringbuf_map);
    if (env.ringbuf_fd < 0) {
        perror("Failed to get ringbuf map fd");
        err = env.ringbuf_fd;
        goto out;
    }

out:
    return err;
}

static void cleanup_bpf(void)
{
    int err;
    if (env.ringbuf_fd >= 0)
        close(env.ringbuf_fd);

    env.tc_opts.flags = env.tc_opts.prog_fd = env.tc_opts.prog_id = 0;
    if (!env.is_xdp) {
        pr_debug("Detaching the TC hook\n");
        err = bpf_tc_detach(&env.tc_hook, &env.tc_opts);
        if (err)
            perror("Failed to detach TC hook\n");
    } else {
        pr_debug("Destroying the TC hook\n");
        err = bpf_tc_hook_destroy(&env.tc_hook);
        if (err)
            perror("Failed to destroy TC hook");
    }
    neighsnoopd_bpf__destroy(env.skel);
}

// epoll setup and cleanup
static int setup_epoll(void)
{
    int err = 0;
    struct epoll_event event;

    env.epoll_fd = epoll_create1(0);

    // Add signalfd to epoll
    event.events = EPOLLIN;
    event.data.fd = env.signal_fd;
    if (epoll_ctl(env.epoll_fd, EPOLL_CTL_ADD, env.signal_fd, &event) == -1) {
        perror("epoll_ctl: signal_fd");
        err = errno;
        goto out;
    }

    // Add netlink socket to epoll
    event.events = EPOLLIN;
    event.data.fd = env.nl_fd;
    if (epoll_ctl(env.epoll_fd, EPOLL_CTL_ADD, env.nl_fd, &event) == -1) {
        perror("epoll_ctl: nl_fd");
        err = errno;
        goto out;
    }

    // Add BPF ring buffer to epoll
    event.events = EPOLLIN;
    event.data.fd = env.ringbuf_fd;
    if (epoll_ctl(env.epoll_fd, EPOLL_CTL_ADD, env.ringbuf_fd, &event) == -1) {
        perror("epoll_ctl: ringbuf_fd");
        err = errno;
        goto out;
    }

    // Add timerfd to epoll
    event.events = EPOLLIN;
    event.data.fd = env.timerfd_fd;
    if (epoll_ctl(env.epoll_fd, EPOLL_CTL_ADD, env.timerfd_fd, &event) == -1) {
        perror("epoll_ctl: timerfd_fd");
        err = errno;
        goto out;
    }

    event.events = EPOLLIN;
    event.data.fd = env.stats_server_fd;
    if (epoll_ctl(env.epoll_fd, EPOLL_CTL_ADD,
                  env.stats_server_fd, &event) == -1) {
        perror("epoll_ctl: stats_server_fd");
        err = errno;
        goto out;
    }

out:
    return err;
}

static void cleanup_epoll(void)
{
    if (env.epoll_fd >= 0)
        close(env.epoll_fd);
}

static int setup_filters(void)
{
    int ret = 0;
    if (env.has_deny_filter) {
        ret = regcomp(&env.deny_filter, env.str_deny_filter, REG_EXTENDED);
        if (ret) {
            perror("Failed to compile regular expression");
            goto out;
        }
    }

out:
    return ret;
}

static void cleanup_filters(void)
{
    if (env.has_deny_filter)
        regfree(&env.deny_filter);
}

static int setup_packet(void)
{
    env.packet_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (env.packet_fd == -1) {
        perror("Failed to open packet socket");
        return errno;
    }
    return 0;
}

static void cleanup_packet(void)
{
    if (env.packet_fd >= 0)
        close(env.packet_fd);
}

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
    static int pos_args;

    switch (key) {
        case 'h':
            argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
            break;
        case '4':
            if (env.only_ipv6) {
                fprintf(stderr, "Cannot specify both --ipv4 and --ipv6\n");
                argp_usage(state);
                exit(EXIT_FAILURE);
            }
            env.only_ipv4 = true;
            break;
        case '6':
            if (env.only_ipv4) {
                fprintf(stderr, "Cannot specify both --ipv4 and --ipv6\n");
                argp_usage(state);
                exit(EXIT_FAILURE);
            }
            env.only_ipv6 = true;
            break;
        case 'c':
            env.has_count = true;
            env.count = strtoul(arg, NULL, 0);
            if (env.count == 0) {
                perror("Invalid count");
                argp_usage(state);
                exit(EXIT_FAILURE);
            }
            break;
        case 'f':
            if (strlen(arg) == 0) {
                fprintf(stderr, "Invalid filter\n");
                argp_usage(state);
                exit(EXIT_FAILURE);
            }
            env.str_deny_filter = arg;
            env.has_deny_filter = true;
            break;
        case 'l':
            env.disable_ipv6ll_filter = true;
            break;
        case 'q':
            env.fail_on_qfilter_present = true;
            break;
        case 'v':
            if (env.debug)
                env.netlink = true;
            if (env.verbose)
                env.debug = true;
            env.verbose = true;
            break;
        case 'x':
            env.is_xdp = true;
            break;
        case ARGP_KEY_NO_ARGS:
            fprintf(stderr, "Missing network device <IFNAME_MON>\n");
            argp_usage(state);
            break;
        case ARGP_KEY_ARG:
            if (pos_args > 0) {
                fprintf(stderr, "Too many arguments: %s\n", arg);
                argp_usage(state);
                exit(EXIT_FAILURE);
            }
            env.ifidx_mon = if_nametoindex(arg);
            if (!env.ifidx_mon) {
                perror("Invalid network device");
                argp_usage(state);
                exit(EXIT_FAILURE);
            }
            strncpy(env.ifidx_mon_str, arg, sizeof(env.ifidx_mon_str));
            pos_args++;
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
// This function is references by argp and not from this code
static void short_usage(FILE *fp, struct argp_state *state)
{
    fprintf(stderr, "Usage: %s [--help] [--verbose] <IFNAME_MON>\n",
            state->argv[0]);
}
#pragma GCC diagnostic pop

int main(int argc, char **argv)
{
    int err;

    static const struct argp argp = {
        .options = opts,
        .parser = parse_arg,
        .doc = argp_program_doc,
        .args_doc = "<IFNAME_MON>",
    };

    err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
    if (err) {
        err = EXIT_FAILURE;
        goto cleanup0;
    }

    if (setup_filters()) {
        err = EXIT_FAILURE;
        goto cleanup0;
    }
    if (setup_packet()) {
        err = EXIT_FAILURE;
        goto cleanup1;
    }
    if (setup_cache()) {
        err = EXIT_FAILURE;
        goto cleanup2;
    }
    if (setup_signals()) {
        err = EXIT_FAILURE;
        goto cleanup3;
    }
    if (setup_netlink()) {
        err = EXIT_FAILURE;
        goto cleanup4;
    }
    if (setup_bpf()) {
        err = EXIT_FAILURE;
        goto cleanup5;
    }
    if (setup_timerfd()) {
        err = EXIT_FAILURE;
        goto cleanup6;
    }
    if (setup_stats()) {
        err = EXIT_FAILURE;
        goto cleanup7;
    }
    if (setup_epoll()) {
        err = EXIT_FAILURE;
        goto cleanup8;
    }

    // Main loop
    main_loop();

    // Cleanup
    cleanup_epoll();
cleanup8:
    cleanup_stats();
cleanup7:
    cleanup_timerfd();
cleanup6:
    cleanup_bpf();
cleanup5:
    cleanup_netlink();
cleanup4:
    cleanup_signals();
cleanup3:
    cleanup_cache();
cleanup2:
    cleanup_packet();
cleanup1:
    cleanup_filters();
cleanup0:
    return err;
}
