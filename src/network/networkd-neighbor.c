/* SPDX-License-Identifier: LGPL-2.1+ */

#include "sd-netlink.h"

#include "alloc-util.h"
#include "conf-parser.h"
#include "ether-addr-util.h"
#include "hashmap.h"
#include "in-addr-util.h"
#include "netlink-util.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-neighbor.h"

void neighbor_free(Neighbor *neighbor) {
        if (!neighbor)
                return;

        if (neighbor->network) {
                LIST_REMOVE(neighbors, neighbor->network->neighbors, neighbor);
                assert(neighbor->network->n_neighbors > 0);
                neighbor->network->n_neighbors--;

                if (neighbor->section) {
                        hashmap_remove(neighbor->network->neighbors_by_section, neighbor->section);
                        network_config_section_free(neighbor->section);
                }
        }

        free(neighbor);
}

static int neighbor_new_static(Network *network, const char *filename, unsigned section_line, Neighbor **ret) {
        _cleanup_(network_config_section_freep) NetworkConfigSection *n = NULL;
        _cleanup_(neighbor_freep) Neighbor *neighbor = NULL;
        int r;

        assert(network);
        assert(ret);
        assert(!!filename == (section_line > 0));

        if (filename) {
                r = network_config_section_new(filename, section_line, &n);
                if (r < 0)
                        return r;

                neighbor = hashmap_get(network->neighbors_by_section, n);
                if (neighbor) {
                        *ret = TAKE_PTR(neighbor);

                        return 0;
                }
        }

        neighbor = new(Neighbor, 1);
        if (!neighbor)
                return -ENOMEM;

        *neighbor = (Neighbor) {
                .network = network,
                .family = AF_UNSPEC,
                .lladdr_type = _NEIGHBOR_LLADDR_INVALID,
        };

        LIST_APPEND(neighbors, network->neighbors, neighbor);
        network->n_neighbors++;

        if (filename) {
                neighbor->section = TAKE_PTR(n);

                r = hashmap_ensure_allocated(&network->neighbors_by_section, &network_config_hash_ops);
                if (r < 0)
                        return r;

                r = hashmap_put(network->neighbors_by_section, neighbor->section, neighbor);
                if (r < 0)
                        return r;
        }

        *ret = TAKE_PTR(neighbor);

        return 0;
}

static int neighbor_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(link);
        assert(link->neighbor_messages > 0);

        link->neighbor_messages--;

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 1;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                /* Neighbor may not exist yet. So, do not enter failed state here. */
                log_link_warning_errno(link, r, "Could not set neighbor, ignoring: %m");

        if (link->neighbor_messages == 0) {
                log_link_debug(link, "Neighbors set");
                link->neighbors_configured = true;
                link_check_ready(link);
        }

        return 1;
}

int neighbor_configure(Neighbor *neighbor, Link *link, link_netlink_message_handler_t callback) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        int r;

        assert(neighbor);
        assert(link);
        assert(link->ifindex > 0);
        assert(link->manager);
        assert(link->manager->rtnl);

        r = sd_rtnl_message_new_neigh(link->manager->rtnl, &req, RTM_NEWNEIGH,
                                          link->ifindex, neighbor->family);
        if (r < 0)
                return log_error_errno(r, "Could not allocate RTM_NEWNEIGH message: %m");

        r = sd_rtnl_message_neigh_set_state(req, NUD_PERMANENT);
        if (r < 0)
                return log_error_errno(r, "Could not set state: %m");

        r = sd_netlink_message_set_flags(req, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE);
        if (r < 0)
                return log_error_errno(r, "Could not set flags: %m");

        if (neighbor->lladdr_type == NEIGHBOR_LLADDR_MAC)
                r = sd_netlink_message_append_data(req, NDA_LLADDR, &neighbor->lladdr.mac, sizeof(neighbor->lladdr.mac));
        else
                r = sd_netlink_message_append_data(req, NDA_LLADDR, &neighbor->lladdr.ip.in, sizeof(neighbor->lladdr.ip.in));
        if (r < 0)
                return log_error_errno(r, "Could not append NDA_LLADDR attribute: %m");

        r = netlink_message_append_in_addr_union(req, NDA_DST, neighbor->family, &neighbor->in_addr);
        if (r < 0)
                return log_error_errno(r, "Could not append NDA_DST attribute: %m");

        r = netlink_call_async(link->manager->rtnl, NULL, req, callback ?: neighbor_handler,
                               link_netlink_destroy_callback, link);
        if (r < 0)
                return log_error_errno(r, "Could not send rtnetlink message: %m");

        link->neighbor_messages++;
        link_ref(link);

        return 0;
}

int neighbor_section_verify(Neighbor *neighbor) {
        if (section_is_invalid(neighbor->section))
                return -EINVAL;

        if (neighbor->family == AF_UNSPEC)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Neighbor section without Address= configured. "
                                         "Ignoring [Neighbor] section from line %u.",
                                         neighbor->section->filename, neighbor->section->line);

        if (neighbor->lladdr_type < 0)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Neighbor section without LinkLayerAddress= configured. "
                                         "Ignoring [Neighbor] section from line %u.",
                                         neighbor->section->filename, neighbor->section->line);

        return 0;
}

int config_parse_neighbor_address(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *network = userdata;
        _cleanup_(neighbor_free_or_set_invalidp) Neighbor *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = neighbor_new_static(network, filename, section_line, &n);
        if (r < 0)
                return r;

        r = in_addr_from_string_auto(rvalue, &n->family, &n->in_addr);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r, "Neighbor Address is invalid, ignoring assignment: %s", rvalue);
                return 0;
        }

        TAKE_PTR(n);

        return 0;
}

int config_parse_neighbor_lladdr(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *network = userdata;
        _cleanup_(neighbor_free_or_set_invalidp) Neighbor *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = neighbor_new_static(network, filename, section_line, &n);
        if (r < 0)
                return r;

        r = ether_addr_from_string(rvalue, &n->lladdr.mac);
        if (r >= 0)
                n->lladdr_type = NEIGHBOR_LLADDR_MAC;
        else {
                r = in_addr_from_string(AF_INET, rvalue, &n->lladdr.ip);
                if (r < 0) {
                        log_syntax(unit, LOG_ERR, filename, line, r, "Neighbor LinkLayerAddress= is invalid, ignoring assignment: %s", rvalue);
                        return 0;
                }
                n->lladdr_type = NEIGHBOR_LLADDR_IP;
        }

        TAKE_PTR(n);

        return 0;
}

int config_parse_neighbor_hwaddr(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *network = userdata;
        _cleanup_(neighbor_free_or_set_invalidp) Neighbor *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = neighbor_new_static(network, filename, section_line, &n);
        if (r < 0)
                return r;

        r = ether_addr_from_string(rvalue, &n->lladdr.mac);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r, "Neighbor MACAddress= is invalid, ignoring assignment: %s", rvalue);
                return 0;
        }

        n->lladdr_type = NEIGHBOR_LLADDR_MAC;
        TAKE_PTR(n);

        return 0;
}
